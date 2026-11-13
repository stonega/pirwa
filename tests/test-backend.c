/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-backend.h"

typedef struct
{
  GMainLoop *loop;
  PirwaReply *reply;
  GError *error;
  GThread *caller;
} Result;

static void
done (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  g_assert_true (g_thread_self () == r->caller);
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
run (PirwaBackend *backend, const PirwaRequest *request, GCancellable *cancel, GError **error)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Result result = { .loop = loop, .caller = g_thread_self () };
  guint timeout = g_timeout_add_seconds (10, timed_out, NULL);
  pirwa_backend_request_async (backend, request, cancel, done, &result);
  g_main_loop_run (loop);
  g_source_remove (timeout);
  if (result.error)
    g_propagate_error (error, result.error);
  return result.reply;
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backend/controls-and-ownership", test_controls);
  g_test_add_func ("/backend/cancellation-and-stop", test_cancellation_and_stop);
  g_test_add_func ("/backend/reconnect-and-missing", test_reconnect_and_missing);
  g_test_add_func ("/backend/lifecycle-events", test_lifecycle_events);
  return g_test_run ();
}
