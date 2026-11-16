/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-backend.h"

/* Observe worker and queue lifetimes without replacing any libvirt operation.
 * Joining belongs only in tests; production must never join on GTK. */
static GThread *worker;
static gint queues;

GThread *__real_g_thread_new (const char *name, GThreadFunc function, gpointer data);
GAsyncQueue *__real_g_async_queue_new_full (GDestroyNotify destroy);
void __real_g_async_queue_unref (GAsyncQueue *queue);

GThread *
__wrap_g_thread_new (const char *name, GThreadFunc function, gpointer data)
{
  GThread *thread = __real_g_thread_new (name, function, data);
  if (g_strcmp0 (name, "pirwa-backend") == 0)
    {
      g_assert_null (worker);
      worker = g_thread_ref (thread);
    }
  return thread;
}

GAsyncQueue *
__wrap_g_async_queue_new_full (GDestroyNotify destroy)
{
  g_atomic_int_inc (&queues);
  return __real_g_async_queue_new_full (destroy);
}

void
__wrap_g_async_queue_unref (GAsyncQueue *queue)
{
  g_atomic_int_add (&queues, -1);
  __real_g_async_queue_unref (queue);
}

typedef struct
{
  GMainLoop *loop;
  GError *error;
} Result;

static void
completed (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *completion = data;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &completion->error);
  g_assert_true ((reply != NULL) == (completion->error == NULL));
  g_main_loop_quit (completion->loop);
}

static void
request (PirwaBackend *backend, PirwaOperation operation, gboolean succeeds)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Result completion = { .loop = loop };
  PirwaRequest command = { .operation = operation };
  pirwa_backend_request_async (backend, &command, NULL, completed, &completion);
  g_main_loop_run (loop);
  if (succeeds)
    g_assert_no_error (completion.error);
  else
    g_assert_nonnull (completion.error);
  g_clear_error (&completion.error);
}

static void
test_lifetime (gconstpointer data)
{
  const char *mode = data;
  gboolean failed = g_str_equal (mode, "failed-connect");
  for (guint round = 0; round < 16; round++)
    {
      PirwaBackend *backend = pirwa_backend_new (failed ? "test:///pirwa-missing-lifetime-fixture"
                                                        : "test:///default");
      GWeakRef reference;
      g_weak_ref_init (&reference, backend);
      if (!g_str_equal (mode, "unopened"))
        {
          request (backend, PIRWA_REFRESH, !failed);
          if (!failed)
            request (backend, PIRWA_RECONNECT, TRUE);
        }
      pirwa_backend_stop (backend);
      g_object_unref (backend);
      g_thread_join (worker);
      worker = NULL;
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_autoptr (GObject) remaining = g_weak_ref_get (&reference);
      g_assert_null (remaining);
      g_weak_ref_clear (&reference);
      /* BackendState owns the queue until its final reference is released. */
      g_assert_cmpint (g_atomic_int_get (&queues), ==, 0);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/backend-lifetime/reconnect", "reconnect", test_lifetime);
  g_test_add_data_func ("/backend-lifetime/failed-connect", "failed-connect", test_lifetime);
  g_test_add_data_func ("/backend-lifetime/unopened", "unopened", test_lifetime);
  return g_test_run ();
}
