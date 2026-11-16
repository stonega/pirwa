/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-backend.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>

/* Keep real test-driver inventories, handles and lifecycle events. Only faults,
 * close notifications and retry waits are supplied by this executable. */
static struct
{
  GThread *main_thread;
  GThread *worker;
  GMutex lock;
  GCond changed;
  virConnectPtr connection;
  virConnectCloseFunc close_callback;
  virFreeCallback close_free;
  void *close_data;
  gint open_failures;
  gint opens;
  gint closes;
  gint registrations;
  gint callback_frees;
  gint indefinite_waits;
  gint retry_waits;
  guint64 delays[32];
  gint hold_retries;
  gint start_fault; /* 1: permission denied; 2: transport lost. */
  gint starts;
  gint dead;
  gint block_stats;
  gint stats_entered;
  gint inventories;
  gint errors;
  gboolean metadata_complete;
  guint64 inventory_serial;
} fixture;

static _Thread_local virError injected_error;
static _Thread_local gboolean error_pending;

/* These fixtures model a persistent QEMU transport using real test-driver
 * domain operations. The test driver itself has no close notifications. */
const char *
__wrap_virConnectGetType (virConnectPtr connection)
{
  (void)connection;
  return "QEMU";
}

virConnectPtr __real_virConnectOpen (const char *uri);
int __real_virConnectClose (virConnectPtr connection);
int __real_virConnectIsAlive (virConnectPtr connection);
int __real_virDomainCreate (virDomainPtr domain);
int __real_virDomainFree (virDomainPtr domain);
virErrorPtr __real_virGetLastError (void);
int __real_virConnectGetAllDomainStats (virConnectPtr connection, unsigned int stats,
                                        virDomainStatsRecordPtr **records, unsigned int flags);
gpointer __real_g_async_queue_pop (GAsyncQueue *queue);
gpointer __real_g_async_queue_timeout_pop (GAsyncQueue *queue, guint64 timeout);
GThread *__real_g_thread_new (const gchar *name, GThreadFunc function, gpointer data);

static void
inject_error (int code, const char *message)
{
  injected_error = (virError){
    .code = code, .domain = VIR_FROM_REMOTE, .level = VIR_ERR_ERROR, .message = (char *)message
  };
  error_pending = TRUE;
}

virErrorPtr
__wrap_virGetLastError (void)
{
  return error_pending ? &injected_error : __real_virGetLastError ();
}

virConnectPtr
__wrap_virConnectOpen (const char *uri)
{
  g_assert_true (g_thread_self () != fixture.main_thread);
  g_assert_cmpstr (uri, ==, "test:///default");
  g_atomic_int_inc (&fixture.opens);
  error_pending = FALSE;
  int failures = g_atomic_int_get (&fixture.open_failures);
  if (failures)
    {
      if (failures > 0)
        g_atomic_int_add (&fixture.open_failures, -1);
      inject_error (VIR_ERR_NO_CONNECT, "Injected daemon unavailable");
      return NULL;
    }
  g_atomic_int_set (&fixture.dead, 0);
  return __real_virConnectOpen (uri);
}

int
__wrap_virConnectClose (virConnectPtr connection)
{
  int result = __real_virConnectClose (connection);
  g_atomic_int_inc (&fixture.closes);
  return result;
}

int
__wrap_virConnectRegisterCloseCallback (virConnectPtr connection, virConnectCloseFunc callback,
                                        void *data, virFreeCallback free_data)
{
  g_mutex_lock (&fixture.lock);
  g_assert_null (fixture.close_callback);
  fixture.connection = connection;
  fixture.close_callback = callback;
  fixture.close_data = data;
  fixture.close_free = free_data;
  g_mutex_unlock (&fixture.lock);
  g_atomic_int_inc (&fixture.registrations);
  return 0;
}

int
__wrap_virConnectUnregisterCloseCallback (virConnectPtr connection, virConnectCloseFunc callback)
{
  g_mutex_lock (&fixture.lock);
  g_assert_true (fixture.connection == connection);
  g_assert_true (fixture.close_callback == callback);
  virFreeCallback free_data = fixture.close_free;
  void *data = fixture.close_data;
  fixture.connection = NULL;
  fixture.close_callback = NULL;
  fixture.close_data = NULL;
  fixture.close_free = NULL;
  g_mutex_unlock (&fixture.lock);
  free_data (data);
  g_atomic_int_inc (&fixture.callback_frees);
  return 0;
}

int
__wrap_virConnectIsAlive (virConnectPtr connection)
{
  g_assert_nonnull (connection);
  return g_atomic_int_get (&fixture.dead) ? 0 : __real_virConnectIsAlive (connection);
}

int
__wrap_virDomainCreate (virDomainPtr domain)
{
  g_atomic_int_inc (&fixture.starts);
  int fault = g_atomic_int_get (&fixture.start_fault);
  if (fault)
    {
      g_atomic_int_set (&fixture.dead, fault == 2);
      inject_error (fault == 1 ? VIR_ERR_OPERATION_DENIED : VIR_ERR_RPC,
                    fault == 1 ? "Injected permission denied" : "Injected transport lost");
      return -1;
    }
  return __real_virDomainCreate (domain);
}

int
__wrap_virDomainFree (virDomainPtr domain)
{
  int result = __real_virDomainFree (domain);
  /* libvirt cleanup can replace the last error; the operation must save it. */
  if (g_atomic_int_get (&fixture.start_fault))
    inject_error (VIR_ERR_INTERNAL_ERROR, "Injected cleanup replaced the last error");
  return result;
}

int
__wrap_virConnectGetAllDomainStats (virConnectPtr connection, unsigned int stats,
                                    virDomainStatsRecordPtr **records, unsigned int flags)
{
  g_assert_true (g_thread_self () != fixture.main_thread);
  g_mutex_lock (&fixture.lock);
  g_atomic_int_inc (&fixture.stats_entered);
  while (g_atomic_int_get (&fixture.block_stats))
    g_cond_wait (&fixture.changed, &fixture.lock);
  g_mutex_unlock (&fixture.lock);
  return __real_virConnectGetAllDomainStats (connection, stats, records, flags);
}

gpointer
__wrap_g_async_queue_pop (GAsyncQueue *queue)
{
  g_atomic_int_inc (&fixture.indefinite_waits);
  return __real_g_async_queue_pop (queue);
}

gpointer
__wrap_g_async_queue_timeout_pop (GAsyncQueue *queue, guint64 timeout)
{
  int index = g_atomic_int_get (&fixture.retry_waits);
  g_assert_cmpint (index, <, (int)G_N_ELEMENTS (fixture.delays));
  fixture.delays[index] = timeout;
  g_atomic_int_inc (&fixture.retry_waits);
  /* Holding a retry still lets queued requests and Stop wake the real queue. */
  return __real_g_async_queue_timeout_pop (
      queue, g_atomic_int_get (&fixture.hold_retries) ? 30 * G_USEC_PER_SEC : 1000);
}

GThread *
__wrap_g_thread_new (const gchar *name, GThreadFunc function, gpointer data)
{
  GThread *thread = __real_g_thread_new (name, function, data);
  if (g_strcmp0 (name, "pirwa-backend") == 0)
    {
      g_assert_null (fixture.worker);
      fixture.worker = g_thread_ref (thread);
    }
  return thread;
}

typedef struct
{
  gint done;
  PirwaReply *reply;
  GError *error;
  guint64 serial;
} Result;

typedef struct
{
  gint *value;
  int target;
  GMainLoop *loop;
} Await;

static gboolean
check_progress (gpointer data)
{
  Await *await = data;
  if (g_atomic_int_get (await->value) < await->target)
    return G_SOURCE_CONTINUE;
  g_main_loop_quit (await->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
timed_out (gpointer data)
{
  (void)data;
  g_error ("Backend fault test did not make progress");
  return G_SOURCE_REMOVE;
}

static void
wait_for (gint *value, int target)
{
  if (g_atomic_int_get (value) >= target)
    return;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Await await = { value, target, loop };
  g_timeout_add (1, check_progress, &await);
  guint watchdog = g_timeout_add_seconds (5, timed_out, NULL);
  g_main_loop_run (loop);
  g_source_remove (watchdog);
}

static void
completed (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *response = data;
  g_assert_true (g_thread_self () == fixture.main_thread);
  response->serial = pirwa_backend_result_serial (result);
  response->reply = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &response->error);
  g_atomic_int_set (&response->done, 1);
}

static Result
request (PirwaBackend *backend, PirwaOperation operation, const char *uuid, GCancellable *cancel)
{
  PirwaRequest command = { .operation = operation, .uuid = uuid };
  Result result = { 0 };
  pirwa_backend_request_async (backend, &command, cancel, completed, &result);
  wait_for (&result.done, 1);
  return result;
}

static void
clear_result (Result *result)
{
  g_clear_pointer (&result->reply, pirwa_reply_free);
  g_clear_error (&result->error);
}

static void
inventory (PirwaBackend *backend, PirwaReply *reply, gpointer data)
{
  (void)backend;
  (void)data;
  g_assert_true (g_thread_self () == fixture.main_thread);
  g_assert_nonnull (reply->machines);
  fixture.metadata_complete = reply->host && reply->pools;
  fixture.inventory_serial = reply->serial;
  g_atomic_int_inc (&fixture.inventories);
}

static void
backend_error (PirwaBackend *backend, guint64 serial, const char *message, gpointer data)
{
  (void)backend;
  (void)data;
  g_assert_true (g_thread_self () == fixture.main_thread);
  g_assert_cmpuint (serial, >, 0);
  g_assert_nonnull (message);
  g_atomic_int_inc (&fixture.errors);
}

static PirwaBackend *
new_backend (void)
{
  PirwaBackend *backend = pirwa_backend_new ("test:///default");
  g_signal_connect (backend, "inventory", G_CALLBACK (inventory), NULL);
  g_signal_connect (backend, "error", G_CALLBACK (backend_error), NULL);
  return backend;
}

static void
stop_backend (PirwaBackend *backend)
{
  pirwa_backend_stop (backend);
  /* Joining is test-only: production shutdown must never join on GTK. */
  g_thread_join (fixture.worker);
  fixture.worker = NULL;
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_null (fixture.close_callback);
  g_assert_cmpint (fixture.callback_frees, ==, fixture.registrations);
  g_assert_cmpint (fixture.closes, ==, fixture.registrations);
}

static gboolean
isolated_test (void)
{
  if (g_test_subprocess ())
    return TRUE;
  g_test_trap_subprocess (NULL, 10 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed ();
  return FALSE;
}

static void
test_startup_recovery (void)
{
  if (!isolated_test ())
    return;
  g_atomic_int_set (&fixture.open_failures, 2);
  g_autoptr (PirwaBackend) backend = new_backend ();
  Result initial = request (backend, PIRWA_REFRESH, NULL, NULL);
  g_assert_null (initial.reply);
  g_assert_error (initial.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  wait_for (&fixture.inventories, 1);
  g_assert_cmpint (fixture.opens, ==, 3);
  g_assert_cmpint (fixture.errors, ==, 1);
  g_assert_cmpuint (fixture.inventory_serial, >, initial.serial);
  g_assert_cmpuint (fixture.delays[0], ==, G_USEC_PER_SEC);
  g_assert_cmpuint (fixture.delays[1], ==, 2 * G_USEC_PER_SEC);
  g_assert_true (fixture.metadata_complete);
  clear_result (&initial);
  stop_backend (backend);
}

static void
assert_backoff (int offset)
{
  for (int i = 0; i < 5; i++)
    g_assert_cmpuint (fixture.delays[offset + i], ==, ((guint64)1 << i) * G_USEC_PER_SEC);
}

static void
test_retry_budget (void)
{
  if (!isolated_test ())
    return;
  g_atomic_int_set (&fixture.open_failures, -1);
  g_autoptr (PirwaBackend) backend = new_backend ();
  Result initial = request (backend, PIRWA_REFRESH, NULL, NULL);
  wait_for (&fixture.indefinite_waits, 2);
  wait_for (&fixture.errors, 5);
  g_assert_cmpint (fixture.opens, ==, 6);
  g_assert_cmpint (fixture.retry_waits, ==, 5);
  assert_backoff (0);
  /* Manual reconnect must start a fresh budget even if still unavailable. */
  Result manual = request (backend, PIRWA_RECONNECT, NULL, NULL);
  g_assert_error (manual.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  wait_for (&fixture.indefinite_waits, 3);
  wait_for (&fixture.errors, 10);
  g_assert_cmpint (fixture.opens, ==, 12);
  g_assert_cmpint (fixture.retry_waits, ==, 10);
  assert_backoff (5);
  g_atomic_int_set (&fixture.open_failures, 0);
  Result recovered = request (backend, PIRWA_RECONNECT, NULL, NULL);
  g_assert_no_error (recovered.error);
  g_assert_nonnull (recovered.reply->machines);
  g_assert_nonnull (recovered.reply->pools);
  g_assert_cmpuint (recovered.serial, >, manual.serial);
  clear_result (&initial);
  clear_result (&manual);
  clear_result (&recovered);
  stop_backend (backend);
}

static void
test_cancelled_backoff (void)
{
  if (!isolated_test ())
    return;
  g_atomic_int_set (&fixture.open_failures, -1);
  g_atomic_int_set (&fixture.hold_retries, 1);
  g_autoptr (PirwaBackend) backend = new_backend ();
  Result initial = request (backend, PIRWA_REFRESH, NULL, NULL);
  wait_for (&fixture.retry_waits, 1);
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  Result cancelled = request (backend, PIRWA_REFRESH, NULL, cancel);
  g_assert_error (cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  wait_for (&fixture.retry_waits, 2);
  g_assert_cmpint (fixture.opens, ==, 1);
  g_assert_cmpuint (fixture.delays[1], ==, fixture.delays[0]);
  clear_result (&initial);
  clear_result (&cancelled);
  stop_backend (backend); /* Stop wakes an outstanding backoff without a timer. */
  g_assert_cmpint (fixture.opens, ==, 1);
}

static void
test_close_notification (void)
{
  if (!isolated_test ())
    return;
  g_atomic_int_set (&fixture.hold_retries, 1);
  g_autoptr (PirwaBackend) backend = new_backend ();
  Result initial = request (backend, PIRWA_REFRESH, NULL, NULL);
  g_assert_no_error (initial.error);
  g_atomic_int_set (&fixture.open_failures, -1);
  g_mutex_lock (&fixture.lock);
  virConnectCloseFunc callback = fixture.close_callback;
  virConnectPtr connection = fixture.connection;
  void *data = fixture.close_data;
  g_mutex_unlock (&fixture.lock);
  g_assert_nonnull (callback);
  callback (connection, VIR_CONNECT_CLOSE_REASON_EOF, data);
  wait_for (&fixture.retry_waits, 1);
  wait_for (&fixture.errors, 1);
  g_assert_cmpint (fixture.closes, ==, 1);
  g_atomic_int_set (&fixture.open_failures, 0);
  Result recovered = request (backend, PIRWA_REFRESH, NULL, NULL);
  g_assert_no_error (recovered.error);
  g_assert_nonnull (recovered.reply->pools);
  g_assert_cmpuint (recovered.serial, >, initial.serial);
  clear_result (&initial);
  clear_result (&recovered);
  stop_backend (backend);
}

static void
test_mutation_failure (gconstpointer data)
{
  if (!isolated_test ())
    return;
  int fault = GPOINTER_TO_INT (data);
  g_autoptr (PirwaBackend) backend = new_backend ();
  Result initial = request (backend, PIRWA_REFRESH, NULL, NULL);
  g_assert_no_error (initial.error);
  PirwaMachine *machine = g_ptr_array_index (initial.reply->machines, 0);
  g_atomic_int_set (&fixture.start_fault, fault);
  Result failed = request (backend, PIRWA_START, pirwa_machine_get_uuid (machine), NULL);
  g_assert_null (failed.reply);
  g_assert_error (failed.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (failed.error->message,
                            fault == 1 ? "Injected permission denied" : "Injected transport lost"));
  if (fault == 2)
    {
      wait_for (&fixture.inventories, 1);
      g_assert_cmpint (fixture.opens, ==, 2);
      g_assert_true (fixture.metadata_complete);
    }
  else
    {
      wait_for (&fixture.indefinite_waits, 3);
      g_assert_cmpint (fixture.opens, ==, 1);
      g_assert_cmpint (fixture.retry_waits, ==, 0);
      Result refreshed = request (backend, PIRWA_REFRESH, NULL, NULL);
      g_assert_no_error (refreshed.error);
      clear_result (&refreshed);
    }
  g_assert_cmpint (fixture.starts, ==, 1); /* Recovery never replays a mutation. */
  clear_result (&initial);
  clear_result (&failed);
  stop_backend (backend);
}

static gboolean
heartbeat (gpointer data)
{
  g_atomic_int_inc ((gint *)data);
  return G_SOURCE_REMOVE;
}

static void
test_blocked_inventory (void)
{
  if (!isolated_test ())
    return;
  g_atomic_int_set (&fixture.block_stats, 1);
  g_autoptr (PirwaBackend) backend = new_backend ();
  PirwaRequest command = { .operation = PIRWA_REFRESH };
  Result pending = { 0 };
  pirwa_backend_request_async (backend, &command, NULL, completed, &pending);
  wait_for (&fixture.stats_entered, 1);
  gint beats = 0;
  g_idle_add (heartbeat, &beats);
  wait_for (&beats, 1);
  g_assert_cmpint (pending.done, ==, 0);
  pirwa_backend_stop (backend);
  g_assert_cmpint (pending.done, ==, 0);
  g_mutex_lock (&fixture.lock);
  g_atomic_int_set (&fixture.block_stats, 0);
  g_cond_broadcast (&fixture.changed);
  g_mutex_unlock (&fixture.lock);
  wait_for (&pending.done, 1);
  g_assert_null (pending.reply);
  g_assert_error (pending.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  stop_backend (backend);
  g_assert_cmpint (fixture.inventories, ==, 0);
  g_assert_cmpint (fixture.errors, ==, 0);
  clear_result (&pending);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  fixture.main_thread = g_thread_self ();
  g_mutex_init (&fixture.lock);
  g_cond_init (&fixture.changed);
  g_test_add_func ("/backend-faults/startup-recovery-metadata", test_startup_recovery);
  g_test_add_func ("/backend-faults/retry-budget-and-manual-reconnect", test_retry_budget);
  g_test_add_func ("/backend-faults/cancelled-work-keeps-budget", test_cancelled_backoff);
  g_test_add_func ("/backend-faults/close-notification", test_close_notification);
  g_test_add_data_func ("/backend-faults/permission-denied", GINT_TO_POINTER (1),
                        test_mutation_failure);
  g_test_add_data_func ("/backend-faults/transport-lost-no-replay", GINT_TO_POINTER (2),
                        test_mutation_failure);
  g_test_add_func ("/backend-faults/blocked-inventory-stop", test_blocked_inventory);
  return g_test_run ();
}
