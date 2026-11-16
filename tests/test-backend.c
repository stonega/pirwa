/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-backend.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  GMainLoop *loop;
  PirwaReply *reply;
  GError *error;
  GThread *caller;
  guint64 serial;
} Result;

static void
done (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  g_assert_true (g_thread_self () == r->caller);
  r->serial = pirwa_backend_result_serial (result);
  r->reply = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &r->error);
  g_main_loop_quit (r->loop);
}

static gboolean
timed_out (gpointer data)
{
  (void)data;
  g_error ("Backend did not complete within 10 seconds");
  return G_SOURCE_REMOVE;
}

static PirwaReply *
run_serial (PirwaBackend *backend, const PirwaRequest *request, GCancellable *cancel,
            guint64 *serial, GError **error)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Result result = { .loop = loop, .caller = g_thread_self () };
  guint timeout = g_timeout_add_seconds (10, timed_out, NULL);
  pirwa_backend_request_async (backend, request, cancel, done, &result);
  g_main_loop_run (loop);
  g_source_remove (timeout);
  if (serial)
    *serial = result.serial;
  if (result.error)
    g_propagate_error (error, result.error);
  return result.reply;
}

static PirwaReply *
run (PirwaBackend *backend, const PirwaRequest *request, GCancellable *cancel, GError **error)
{
  return run_serial (backend, request, cancel, NULL, error);
}

static PirwaMachine *
find_test (PirwaReply *reply)
{
  for (guint i = 0; i < reply->machines->len; i++)
    {
      PirwaMachine *machine = g_ptr_array_index (reply->machines, i);
      if (g_str_equal (pirwa_machine_get_name (machine), "test"))
        return machine;
    }
  g_error ("Default test VM was not found");
  return NULL;
}

static void
test_usage (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GError) error = NULL;
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) inventory = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  request.uuid = pirwa_machine_get_uuid (find_test (inventory));
  request.operation = PIRWA_USAGE;
  g_autoptr (PirwaReply) first = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);
  g_assert_true (first->usage.running);
  /* Drivers may omit optional fields; do not substitute configured resources. */
  if (!first->usage.has_cpu_time)
    g_assert_cmpuint (first->usage.cpu_time_ns, ==, 0);
  if (!first->usage.has_memory_rss)
    g_assert_cmpuint (first->usage.memory_rss_kib, ==, 0);
  if (!first->usage.has_network)
    {
      g_assert_cmpuint (first->usage.network_rx_bytes, ==, 0);
      g_assert_cmpuint (first->usage.network_tx_bytes, ==, 0);
    }
  g_assert_cmpint (first->usage.sampled_at_us, >, 0);
  g_assert_null (first->machines); /* Sampling must not refresh the collection. */
  g_assert_null (first->details);
  request.operation = PIRWA_SHUTDOWN;
  g_autoptr (PirwaReply) stopped = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  request.operation = PIRWA_USAGE;
  g_autoptr (PirwaReply) inactive = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inactive);
  g_assert_false (inactive->usage.running);
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  g_autoptr (PirwaReply) cancelled = run (backend, &request, cancel, &error);
  g_assert_null (cancelled);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  pirwa_backend_stop (backend);
}

static void
test_controls (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GError) error = NULL;
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) initial = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_autofree char *uuid = g_strdup (pirwa_machine_get_uuid (find_test (initial)));
  request.uuid = uuid;
  request.operation = PIRWA_CONSOLE;
  g_autoptr (PirwaReply) console = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (console->viewer);
  g_assert_true (g_subprocess_wait_check (console->viewer, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (console->native_console);
  request.operation = PIRWA_GRAPHICS_FD;
  g_autoptr (PirwaReply) unsupported = run (backend, &request, NULL, &error);
  g_assert_null (unsupported);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  const PirwaOperation ops[]
      = { PIRWA_PAUSE, PIRWA_RESUME, PIRWA_REBOOT, PIRWA_SHUTDOWN, PIRWA_START, PIRWA_FORCE_STOP };
  const int states[] = { 3, 1, 1, 5, 1, 5 };
  for (guint i = 0; i < G_N_ELEMENTS (ops); i++)
    {
      request.operation = ops[i];
      g_autoptr (PirwaReply) reply = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (reply);
      g_assert_cmpint (pirwa_machine_get_state_code (find_test (reply)), ==, states[i]);
    }
  request.operation = PIRWA_EDIT;
  request.options.name = "test";
  request.options.cpus = 3;
  request.options.memory_mb = 2048;
  g_autoptr (PirwaReply) edited = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (edited);
  request.operation = PIRWA_DETAILS;
  g_autoptr (PirwaReply) details = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (details->details->cpus, ==, 3);
  g_assert_cmpuint (details->details->memory_mb, ==, 2048);
  request.operation = PIRWA_REMOVE;
  request.delete_disk = TRUE;
  g_autoptr (PirwaReply) rejected = run (backend, &request, NULL, &error);
  g_assert_null (rejected);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&error);
  request.delete_disk = FALSE;
  g_autoptr (PirwaReply) removed = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (removed->machines->len, ==, 0);
  pirwa_backend_stop (backend);
}

static void
test_cancellation_and_stop (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_cancellable_cancel (cancel);
  g_autoptr (PirwaReply) reply = run (backend, &request, cancel, &error);
  g_assert_null (reply);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  pirwa_backend_stop (backend);
  reply = run (backend, &request, NULL, &error);
  g_assert_null (reply);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_reconnect_and_missing (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GError) error = NULL;
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) reply = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reply);
  request.operation = PIRWA_RECONNECT;
  g_autoptr (PirwaReply) again = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (again);
  request.operation = PIRWA_START;
  request.uuid = "00000000-0000-0000-0000-000000000001";
  g_autoptr (PirwaReply) missing = run (backend, &request, NULL, &error);
  g_assert_null (missing);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  pirwa_backend_stop (backend);
}

typedef struct
{
  GMainLoop *loop;
  gboolean seen;
} EventResult;
static void
inventory_event (PirwaBackend *backend, PirwaReply *reply, gpointer data)
{
  (void)backend;
  EventResult *event = data;
  if (reply->machines && reply->machines->len
      && pirwa_machine_get_state_code (find_test (reply)) == 3)
    {
      event->seen = TRUE;
      if (event->loop)
        g_main_loop_quit (event->loop);
    }
}

static void
test_lifecycle_events (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GError) error = NULL;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  EventResult event = { .loop = loop };
  g_signal_connect (backend, "inventory", G_CALLBACK (inventory_event), &event);
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) initial = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  request.uuid = pirwa_machine_get_uuid (find_test (initial));
  request.operation = PIRWA_PAUSE;
  g_autoptr (PirwaReply) paused = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  if (!event.seen)
    {
      guint timeout = g_timeout_add_seconds (10, timed_out, NULL);
      g_main_loop_run (loop);
      g_source_remove (timeout);
    }
  g_assert_true (event.seen);
  g_signal_handlers_disconnect_by_data (backend, &event);
  pirwa_backend_stop (backend);
}

typedef struct
{
  PirwaBackend *backend;
  const char *path;
  gboolean stopped_during_connect;
} BlockedConnect;

static gboolean
release_blocked_connect (gpointer data)
{
  BlockedConnect *blocked = data;
  int fd = open (blocked->path, O_WRONLY | O_NONBLOCK);
  if (fd < 0 && errno == ENXIO)
    return G_SOURCE_CONTINUE;
  g_assert_cmpint (fd, >=, 0);
  pirwa_backend_stop (blocked->backend);
  blocked->stopped_during_connect = TRUE;
  const char node[] = "<node/>";
  g_assert_cmpint (write (fd, node, sizeof node - 1), ==, sizeof node - 1);
  close (fd);
  return G_SOURCE_REMOVE;
}

static void
test_stop_in_flight (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp ("pirwa-blocked-connect-XXXXXX", &error);
  g_assert_no_error (error);
  g_autofree char *path = g_build_filename (directory, "node.xml", NULL);
  g_assert_cmpint (mkfifo (path, 0600), ==, 0);
  g_autofree char *uri = g_strconcat ("test://", path, NULL);
  g_autoptr (PirwaBackend) backend = pirwa_backend_new (uri);
  BlockedConnect blocked = { .backend = backend, .path = path };
  guint release = g_timeout_add (5, release_blocked_connect, &blocked);
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) reply = run (backend, &request, NULL, &error);
  if (!blocked.stopped_during_connect)
    g_source_remove (release);
  g_assert_true (blocked.stopped_during_connect);
  g_assert_null (reply);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_unlink (path);
  g_rmdir (directory);
}

static void
progress_id (PirwaBackend *backend, guint64 id, double fraction, const char *stage, gpointer data)
{
  (void)backend;
  g_assert_cmpfloat (fraction, >=, 0);
  g_assert_nonnull (stage);
  g_array_append_val ((GArray *)data, id);
}

static void
test_result_order_and_progress (void)
{
  g_autoptr (PirwaBackend) backend = pirwa_backend_new ("test:///default");
  g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_signal_connect (backend, "progress", G_CALLBACK (progress_id), ids);
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;
  int fd = g_file_open_tmp ("pirwa-media-XXXXXX", &path, &error);
  g_assert_no_error (error);
  close (fd);
  PirwaRequest request
      = { .operation = PIRWA_CREATE,
          .options
          = { .name = "test-create", .source = path, .cpus = 1, .memory_mb = 256, .disk_gb = 4 } };
  guint64 previous = 0;
  for (guint64 id = 41; id < 43; id++)
    {
      request.request_id = id;
      guint64 serial = 0;
      /* The built-in test driver has no KVM domain capabilities, so this is
       * a deterministic pre-allocation failure after the first progress event. */
      g_autoptr (PirwaReply) reply = run_serial (backend, &request, NULL, &serial, &error);
      g_assert_null (reply);
      g_assert_nonnull (error);
      g_assert_cmpuint (serial, >, previous);
      previous = serial;
      g_clear_error (&error);
    }
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_cmpuint (ids->len, ==, 2);
  g_assert_cmpuint (g_array_index (ids, guint64, 0), ==, 41);
  g_assert_cmpuint (g_array_index (ids, guint64, 1), ==, 42);
  g_signal_handlers_disconnect_by_data (backend, ids);
  pirwa_backend_stop (backend);
  g_unlink (path);
}

static void
test_media (void)
{
  const int states[] = { 1, 3, 5 };
  for (guint i = 0; i < G_N_ELEMENTS (states); i++)
    {
      g_autofree char *path = NULL;
      g_autoptr (GError) error = NULL;
      int fd = g_file_open_tmp ("pirwa-media-XXXXXX", &path, &error);
      g_assert_no_error (error);
      close (fd);
      g_autofree char *xml = g_strdup_printf (
          "<node><domain type='test'><name>test</name>"
          "<uuid>22222222-2222-4222-8222-222222222222</uuid>"
          "<memory unit='KiB'>262144</memory><vcpu>1</vcpu><os><type>hvm</type></os>"
          "<test:runstate xmlns:test='http://libvirt.org/schemas/domain/test/1.0'>"
          "%d</test:runstate><devices><disk type='file' device='cdrom'>"
          "<source file='/images/Fedora &amp; Test.iso'/><target dev='sda' bus='sata'/>"
          "<readonly/></disk></devices></domain></node>",
          states[i]);
      g_assert_true (g_file_set_contents (path, xml, -1, &error));
      g_autofree char *uri = g_strconcat ("test://", path, NULL);
      g_autoptr (PirwaBackend) backend = pirwa_backend_new (uri);
      PirwaRequest request
          = { .operation = PIRWA_DETAILS, .uuid = "22222222-2222-4222-8222-222222222222" };
      g_autoptr (PirwaReply) initial = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpstr (initial->details->media_source, ==, "/images/Fedora & Test.iso");
      request.operation = PIRWA_EJECT;
      request.media_target = "sda";
      request.media_source = "/images/stale.iso";
      g_autoptr (PirwaReply) stale = run (backend, &request, NULL, &error);
      g_assert_null (stale);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
      g_clear_error (&error);
      request.media_source = initial->details->media_source;
      g_autoptr (GCancellable) cancel = g_cancellable_new ();
      g_cancellable_cancel (cancel);
      g_autoptr (PirwaReply) cancelled = run (backend, &request, cancel, &error);
      g_assert_null (cancelled);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error (&error);
      g_autoptr (PirwaReply) ejected = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (ejected);
      request.operation = PIRWA_DETAILS;
      g_autoptr (PirwaReply) current = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      g_assert_null (current->details->media_source);
      g_assert_false (current->details->has_cdrom);
      /* An active tray and the next boot must both stay empty. */
      if (states[i] != 5)
        {
          request.operation = PIRWA_FORCE_STOP;
          g_autoptr (PirwaReply) stopped = run (backend, &request, NULL, &error);
          g_assert_no_error (error);
        }
      request.operation = PIRWA_START;
      g_autoptr (PirwaReply) started = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      request.operation = PIRWA_DETAILS;
      g_autoptr (PirwaReply) rebooted = run (backend, &request, NULL, &error);
      g_assert_no_error (error);
      g_assert_null (rebooted->details->media_source);
      g_assert_cmpuint (rebooted->details->cpus, ==, initial->details->cpus);
      g_assert_cmpuint (rebooted->details->memory_mb, ==, initial->details->memory_mb);
      pirwa_backend_stop (backend);
      g_unlink (path);
    }
}

static void
test_native_console (void)
{
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  int fd = g_file_open_tmp ("pirwa-spice-XXXXXX", &path, &error);
  g_assert_no_error (error);
  close (fd);
  const char *xml = "<node><domain type='test'><name>test</name>"
                    "<uuid>22222222-2222-4222-8222-222222222222</uuid>"
                    "<memory unit='KiB'>262144</memory><vcpu>1</vcpu>"
                    "<os><type>hvm</type></os><devices>"
                    "<graphics type='spice'/></devices></domain></node>";
  g_assert_true (g_file_set_contents (path, xml, -1, &error));
  g_autofree char *uri = g_strconcat ("test://", path, NULL);
  g_autoptr (PirwaBackend) backend = pirwa_backend_new (uri);
  PirwaRequest request
      = { .operation = PIRWA_CONSOLE, .uuid = "22222222-2222-4222-8222-222222222222" };
  g_autoptr (PirwaReply) reply = run (backend, &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reply);
  g_assert_true (reply->native_console);
  g_assert_cmpstr (reply->console_name, ==, "test");
  g_assert_null (reply->viewer);
  g_assert_null (reply->uuid);
  g_assert_null (reply->machines);
  request.operation = PIRWA_GRAPHICS_FD;
  g_autoptr (PirwaReply) socket = run (backend, &request, NULL, &error);
  g_assert_null (socket); /* The synthetic driver cannot supply graphics sockets. */
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  pirwa_backend_stop (backend);
  g_unlink (path);
}

static void
test_socket_ownership (void)
{
  int sockets[2];
  g_assert_cmpint (pipe (sockets), ==, 0);
  PirwaReply *reply = g_new0 (PirwaReply, 1);
  reply->graphics_fds = g_unix_fd_list_new_from_array (sockets, 2);
  pirwa_reply_free (reply);
  errno = 0;
  g_assert_cmpint (fcntl (sockets[0], F_GETFD), ==, -1);
  g_assert_cmpint (errno, ==, EBADF);
  errno = 0;
  g_assert_cmpint (fcntl (sockets[1], F_GETFD), ==, -1);
  g_assert_cmpint (errno, ==, EBADF);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backend/usage", test_usage);
  g_test_add_func ("/backend/socket-ownership", test_socket_ownership);
  g_test_add_func ("/backend/native-console", test_native_console);
  g_test_add_func ("/backend/media", test_media);
  g_test_add_func ("/backend/controls-and-ownership", test_controls);
  g_test_add_func ("/backend/cancellation-and-stop", test_cancellation_and_stop);
  g_test_add_func ("/backend/reconnect-and-missing", test_reconnect_and_missing);
  g_test_add_func ("/backend/lifecycle-events", test_lifecycle_events);
  g_test_add_func ("/backend/stop-in-flight", test_stop_in_flight);
  g_test_add_func ("/backend/result-order-and-progress", test_result_order_and_progress);
  return g_test_run ();
}
