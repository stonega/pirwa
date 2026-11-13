/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-backend.h"
#include "pirwa-provision.h"
#include <libvirt-glib/libvirt-glib.h>

typedef struct
{
  GTask *task;
  PirwaRequest request;
  gboolean stop;
} Job;

typedef struct
{
  gatomicrefcount refs;
  GWeakRef owner;
  GMainContext *context;
  GAsyncQueue *queue;
  GCancellable *shutdown;
  char *uri;
  virConnectPtr connection;
  int event_id;
  gboolean close_registered;
  gint stopping;
  gint refresh_pending;
  guint retries;
  gint64 last_progress;
  guint64 serial;
  guint64 active_request;
} BackendState;

struct _PirwaBackend
{
  GObject parent_instance;
  BackendState *state;
};

G_DEFINE_FINAL_TYPE (PirwaBackend, pirwa_backend, G_TYPE_OBJECT)

void
pirwa_reply_free (PirwaReply *reply)
{
  if (!reply)
    return;
  g_clear_pointer (&reply->machines, g_ptr_array_unref);
  g_strfreev (reply->pools);
  pirwa_details_free (reply->details);
  g_clear_object (&reply->viewer);
  g_free (reply->uuid);
  g_free (reply->host);
  g_free (reply);
}

static void
job_free (gpointer data)
{
  Job *job = data;
  g_clear_object (&job->task);
  g_free ((char *)job->request.uuid);
  g_free (job->request.options.name);
  g_free (job->request.options.source);
  g_free (job->request.options.pool);
  g_free (job);
}

static BackendState *
state_ref (BackendState *state)
{
  g_atomic_ref_count_inc (&state->refs);
  return state;
}

static void
state_unref (gpointer data)
{
  BackendState *state = data;
  if (!g_atomic_ref_count_dec (&state->refs))
    return;
  g_weak_ref_clear (&state->owner);
  g_main_context_unref (state->context);
  g_async_queue_unref (state->queue);
  g_object_unref (state->shutdown);
  g_free (state->uri);
  g_free (state);
}

typedef struct
{
  PirwaBackend *backend;
  PirwaReply *reply;
  char *message;
  double fraction;
  gboolean progress;
  guint64 serial;
  guint64 request_id;
} Delivery;

static gboolean
deliver (gpointer data)
{
  Delivery *delivery = data;
  if (!g_atomic_int_get (&delivery->backend->state->stopping))
    {
      if (delivery->progress)
        g_signal_emit_by_name (delivery->backend, "progress", delivery->request_id,
                               delivery->fraction, delivery->message);
      else if (delivery->reply)
        g_signal_emit_by_name (delivery->backend, "inventory", delivery->reply);
      else
        g_signal_emit_by_name (delivery->backend, "error", delivery->serial, delivery->message);
    }
  return G_SOURCE_REMOVE;
}

static void
delivery_free (gpointer data)
{
  Delivery *delivery = data;
  g_object_unref (delivery->backend);
  pirwa_reply_free (delivery->reply);
  g_free (delivery->message);
  g_free (delivery);
}

static void
post (BackendState *state, PirwaReply *reply, const char *message, double fraction)
{
  PirwaBackend *owner = g_weak_ref_get (&state->owner);
  if (!owner)
    {
      pirwa_reply_free (reply);
      return;
    }
  Delivery *delivery = g_new0 (Delivery, 1);
  delivery->backend = owner;
  delivery->serial = state->serial;
  delivery->request_id = state->active_request;
  delivery->reply = reply;
  delivery->message = g_strdup (message);
  delivery->fraction = fraction;
  delivery->progress = fraction >= 0;
  GSource *source = g_idle_source_new ();
  g_source_set_callback (source, deliver, delivery, delivery_free);
  g_source_attach (source, state->context);
  g_source_unref (source);
}

static void
progress (double fraction, const char *stage, gpointer data)
{
  BackendState *state = data;
  gint64 now = g_get_monotonic_time ();
  if (fraction < 1 && fraction > 0.15 && now - state->last_progress < 100000)
    return;
  state->last_progress = now;
  post (state, NULL, stage, fraction);
}

static void
queue_event (BackendState *state, PirwaOperation operation)
{
  if (g_atomic_int_get (&state->stopping))
    return;
  if (operation == PIRWA_REFRESH
      && !g_atomic_int_compare_and_exchange (&state->refresh_pending, 0, 1))
    return;
  Job *job = g_new0 (Job, 1);
  job->request.operation = operation;
  g_async_queue_push (state->queue, job);
}

static int
lifecycle (virConnectPtr connection, virDomainPtr domain, int event, int detail, void *opaque)
{
  (void)connection;
  (void)domain;
  (void)event;
  (void)detail;
  queue_event (opaque, PIRWA_REFRESH);
  return 0;
}

static void
connection_closed (virConnectPtr connection, int reason, void *opaque)
{
  (void)connection;
  (void)reason;
  queue_event (opaque, PIRWA_RECONNECT);
}

static void
ensure_event_loop (void)
{
  static gsize initialized;
  if (g_once_init_enter (&initialized))
    {
      gvir_event_register ();
      g_once_init_leave (&initialized, 1);
    }
}

static void
close_connection (BackendState *state)
{
  if (!state->connection)
    return;
  if (state->close_registered)
    virConnectUnregisterCloseCallback (state->connection, connection_closed);
  if (state->event_id >= 0)
    virConnectDomainEventDeregisterAny (state->connection, state->event_id);
  virConnectClose (state->connection);
  state->connection = NULL;
  state->close_registered = FALSE;
  state->event_id = -1;
}

static gboolean
connect_backend (BackendState *state, GError **error)
{
  if (state->connection)
    return TRUE;
  state->connection = virConnectOpen (state->uri);
  if (!state->connection)
    {
      pirwa_libvirt_error (error, "Cannot connect to libvirt");
      return FALSE;
    }
  state_ref (state);
  state->event_id = virConnectDomainEventRegisterAny (
      state->connection, NULL, VIR_DOMAIN_EVENT_ID_LIFECYCLE, VIR_DOMAIN_EVENT_CALLBACK (lifecycle),
      state, state_unref);
  if (state->event_id < 0)
    {
      state_unref (state);
      pirwa_libvirt_error (error, "Lifecycle events are unavailable");
      close_connection (state);
      return FALSE;
    }
  state_ref (state);
  if (virConnectRegisterCloseCallback (state->connection, connection_closed, state, state_unref)
      == 0)
    state->close_registered = TRUE;
  else
    state_unref (state);
  state->retries = 0;
  return TRUE;
}

static GStrv
list_pools (virConnectPtr connection)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  virStoragePoolPtr *pools = NULL;
  int count = virConnectListAllStoragePools (connection, &pools,
                                             VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE
                                                 | VIR_CONNECT_LIST_STORAGE_POOLS_DIR);
  for (int i = 0; i < count; i++)
    {
      g_ptr_array_add (names, g_strdup (virStoragePoolGetName (pools[i])));
      virStoragePoolFree (pools[i]);
    }
  free (pools);
  g_ptr_array_add (names, NULL);
  return (GStrv)g_ptr_array_free (names, FALSE);
}

static gboolean
edit_domain (virConnectPtr connection, virDomainPtr domain, const PirwaRequest *request,
             GError **error)
{
  if (virDomainIsActive (domain) != 0 || virDomainIsPersistent (domain) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Power off this persistent VM before editing it");
      return FALSE;
    }
  g_autofree char *original = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  if (!original)
    {
      pirwa_libvirt_error (error, "Cannot read definition");
      return FALSE;
    }
  g_autoptr (PirwaDetails) details = pirwa_config_parse (original, error);
  if (!details)
    return FALSE;
  g_autofree char *modified
      = pirwa_config_edit (original, request->options.name, request->options.cpus,
                           request->options.memory_mb, request->eject, error);
  if (!modified)
    return FALSE;
  gboolean rename = g_strcmp0 (details->name, request->options.name) != 0;
  if (rename && virDomainRename (domain, request->options.name, 0) < 0)
    {
      pirwa_libvirt_error (error, "Cannot rename virtual machine");
      return FALSE;
    }
  virDomainPtr updated = virDomainDefineXML (connection, modified);
  if (!updated)
    {
      pirwa_libvirt_error (error, "Cannot save resources");
      if (rename && virDomainRename (domain, details->name, 0) < 0)
        g_prefix_error (error, "The name change could not be rolled back. ");
      return FALSE;
    }
  virDomainFree (updated);
  return TRUE;
}

static PirwaReply *
execute (BackendState *state, const PirwaRequest *request, GCancellable *cancellable,
         gboolean describe_host, GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;
  if (request->operation == PIRWA_RECONNECT)
    close_connection (state);
  if (!connect_backend (state, error))
    return NULL;
  PirwaReply *reply = g_new0 (PirwaReply, 1);
  virDomainPtr domain = NULL;
  if (request->operation >= PIRWA_START && request->operation != PIRWA_CREATE)
    {
      if (!request->uuid || !g_uuid_string_is_valid (request->uuid))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "Invalid VM identity");
          goto fail;
        }
      domain = virDomainLookupByUUIDString (state->connection, request->uuid);
      if (!domain)
        {
          pirwa_libvirt_error (error, "This virtual machine no longer exists");
          goto fail;
        }
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto fail;
  int status = 0;
  switch (request->operation)
    {
    case PIRWA_START:
      status = virDomainCreate (domain);
      break;
    case PIRWA_SHUTDOWN:
      status = virDomainShutdown (domain);
      break;
    case PIRWA_REBOOT:
      status = virDomainReboot (domain, 0);
      break;
    case PIRWA_PAUSE:
      status = virDomainSuspend (domain);
      break;
    case PIRWA_RESUME:
      status = virDomainResume (domain);
      break;
    case PIRWA_FORCE_STOP:
      status = virDomainDestroy (domain);
      break;
    case PIRWA_DETAILS:
      {
        g_autofree char *xml = virDomainGetXMLDesc (
            domain, virDomainIsPersistent (domain) == 1 ? VIR_DOMAIN_XML_INACTIVE : 0);
        if (!xml)
          {
            pirwa_libvirt_error (error, "Cannot read VM details");
            goto fail;
          }
        reply->details = pirwa_config_parse (xml, error);
        if (!reply->details)
          goto fail;
        reply->details->managed
            = pirwa_provision_owned (state->connection, request->uuid, reply->details);
        break;
      }
    case PIRWA_EDIT:
      if (!edit_domain (state->connection, domain, request, error))
        goto fail;
      break;
    case PIRWA_REMOVE:
      if (!pirwa_provision_remove (state->connection, domain, request->uuid, request->delete_disk,
                                   cancellable, error))
        goto fail;
      break;
    case PIRWA_CONSOLE:
      reply->viewer
          = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, error, "virt-viewer", "--connect",
                              state->uri, "--attach", "--reconnect", "--wait", request->uuid, NULL);
      if (!reply->viewer)
        {
          g_prefix_error (error, "Install virt-viewer to open the console. ");
          goto fail;
        }
      break;
    case PIRWA_CREATE:
      reply->uuid = pirwa_provision_create (state->connection, &request->options, cancellable,
                                            progress, state, error);
      if (!reply->uuid)
        goto fail;
      break;
    default:
      break;
    }
  if (status < 0)
    {
      pirwa_libvirt_error (error, "Virtual machine operation failed");
      goto fail;
    }
  if (domain)
    {
      virDomainFree (domain);
      domain = NULL;
    }
  if (request->operation != PIRWA_DETAILS && request->operation != PIRWA_CONSOLE)
    {
      /* A completed mutation stays successful even if the follow-up read fails.
       * Its lifecycle event or manual refresh will reconcile the collection. */
      g_autoptr (GError) read_error = NULL;
      reply->machines = pirwa_inventory_read (state->connection, NULL, &read_error);
      if (!reply->machines && request->operation <= PIRWA_RECONNECT)
        {
          g_propagate_error (error, g_steal_pointer (&read_error));
          goto fail;
        }
      if (describe_host)
        {
          reply->pools = list_pools (state->connection);
          virNodeInfo host;
          if (virNodeGetInfo (state->connection, &host) == 0)
            reply->host = g_strdup_printf ("%s · %u CPUs · %.1f GiB RAM", host.model, host.cpus,
                                           host.memory / 1048576.0);
        }
    }
  return reply;
fail:
  if (domain)
    virDomainFree (domain);
  pirwa_reply_free (reply);
  return NULL;
}

static void
forward_cancel (GCancellable *source, gpointer destination)
{
  (void)source;
  g_cancellable_cancel (destination);
}

static gpointer
worker (gpointer data)
{
  BackendState *state = data;
  while (TRUE)
    {
      Job *job;
      if (!state->connection && state->retries > 0 && state->retries <= 5)
        job = g_async_queue_timeout_pop (state->queue,
                                         ((guint64)1 << (state->retries - 1)) * G_USEC_PER_SEC);
      else
        job = g_async_queue_pop (state->queue);
      if (!job)
        {
          job = g_new0 (Job, 1);
          job->request.operation = PIRWA_REFRESH;
        }
      if (job->stop)
        {
          job_free (job);
          break;
        }
      if (job->request.operation == PIRWA_REFRESH && !job->task)
        g_atomic_int_set (&state->refresh_pending, 0);
      g_autoptr (GError) error = NULL;
      g_autoptr (GCancellable) cancellable = g_cancellable_new ();
      GCancellable *caller = job->task ? g_task_get_cancellable (job->task) : NULL;
      gulong shutdown_id
          = g_cancellable_connect (state->shutdown, G_CALLBACK (forward_cancel), cancellable, NULL);
      gulong caller_id
          = caller ? g_cancellable_connect (caller, G_CALLBACK (forward_cancel), cancellable, NULL)
                   : 0;
      PirwaReply *reply = NULL;
      state->active_request = job->request.request_id;
      if (g_atomic_int_get (&state->stopping))
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Connection closed");
      else
        reply = execute (state, &job->request, cancellable,
                         job->task && job->request.operation <= PIRWA_RECONNECT, &error);
      if (caller_id)
        g_cancellable_disconnect (caller, caller_id);
      if (shutdown_id)
        g_cancellable_disconnect (state->shutdown, shutdown_id);
      if (!state->connection)
        state->retries++;
      else if (!reply && virConnectIsAlive (state->connection) == 0)
        {
          close_connection (state);
          state->retries = 1;
        }
      state->serial++;
      if (reply)
        reply->serial = state->serial;
      if (job->task)
        {
          guint64 *serial = g_new (guint64, 1);
          *serial = state->serial;
          g_task_set_task_data (job->task, serial, g_free);
          if (reply)
            g_task_return_pointer (job->task, reply, (GDestroyNotify)pirwa_reply_free);
          else
            g_task_return_error (job->task, g_steal_pointer (&error));
        }
      else
        post (state, reply, error ? error->message : NULL, -1);
      job_free (job);
    }
  close_connection (state);
  state_unref (state);
  return NULL;
}

void
pirwa_backend_stop (PirwaBackend *self)
{
  if (g_atomic_int_compare_and_exchange (&self->state->stopping, 0, 1))
    {
      g_cancellable_cancel (self->state->shutdown);
      Job *job = g_new0 (Job, 1);
      job->stop = TRUE;
      g_async_queue_push (self->state->queue, job);
    }
}

static void
pirwa_backend_dispose (GObject *object)
{
  PirwaBackend *self = PIRWA_BACKEND (object);
  if (self->state)
    pirwa_backend_stop (self);
  G_OBJECT_CLASS (pirwa_backend_parent_class)->dispose (object);
}

static void
pirwa_backend_finalize (GObject *object)
{
  PirwaBackend *self = PIRWA_BACKEND (object);
  if (self->state)
    state_unref (self->state);
  G_OBJECT_CLASS (pirwa_backend_parent_class)->finalize (object);
}

static void
pirwa_backend_class_init (PirwaBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_backend_dispose;
  G_OBJECT_CLASS (klass)->finalize = pirwa_backend_finalize;
  g_signal_new ("inventory", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_POINTER);
  g_signal_new ("error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 2, G_TYPE_UINT64, G_TYPE_STRING);
  g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 3, G_TYPE_UINT64, G_TYPE_DOUBLE, G_TYPE_STRING);
}

static void
pirwa_backend_init (PirwaBackend *self)
{
  (void)self;
}

PirwaBackend *
pirwa_backend_new (const char *uri)
{
  ensure_event_loop ();
  PirwaBackend *self = g_object_new (PIRWA_TYPE_BACKEND, NULL);
  BackendState *state = g_new0 (BackendState, 1);
  g_atomic_ref_count_init (&state->refs);
  g_weak_ref_init (&state->owner, self);
  state->context = g_main_context_ref_thread_default ();
  state->queue = g_async_queue_new_full (job_free);
  state->shutdown = g_cancellable_new ();
  state->uri = g_strdup (uri);
  state->event_id = -1;
  self->state = state;
  g_thread_unref (g_thread_new ("pirwa-backend", worker, state_ref (state)));
  return self;
}

const char *
pirwa_backend_get_uri (PirwaBackend *self)
{
  return self->state->uri;
}

void
pirwa_backend_request_async (PirwaBackend *self, const PirwaRequest *request,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->task = g_task_new (self, cancellable, callback, data);
  /* Do not report a committed mutation as cancelled after the fact. */
  g_task_set_check_cancellable (job->task, FALSE);
  job->request = *request;
  job->request.uuid = g_strdup (request->uuid);
  job->request.options.name = g_strdup (request->options.name);
  job->request.options.source = g_strdup (request->options.source);
  job->request.options.pool = g_strdup (request->options.pool);
  if (g_atomic_int_get (&self->state->stopping))
    {
      g_task_return_new_error (job->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Connection closed");
      job_free (job);
      return;
    }
  g_async_queue_push (self->state->queue, job);
}

PirwaReply *
pirwa_backend_request_finish (PirwaBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

guint64
pirwa_backend_result_serial (GAsyncResult *result)
{
  g_return_val_if_fail (G_IS_TASK (result), 0);
  const guint64 *serial = g_task_get_task_data (G_TASK (result));
  return serial ? *serial : 0;
}
