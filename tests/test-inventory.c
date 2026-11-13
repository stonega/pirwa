/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-inventory.h"

typedef struct
{
  GMainLoop *loop;
  GPtrArray *machines;
  GError *error;
  GThread *caller;
} LoadResult;

static void
loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  LoadResult *load = user_data;
  g_assert_true (g_thread_self () == load->caller);
  load->machines = pirwa_inventory_load_finish (result, &load->error);
  g_main_loop_quit (load->loop);
}

static void
test_discovery (void)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  LoadResult result = { .loop = loop, .caller = g_thread_self () };
  pirwa_inventory_load_async ("test:///default", NULL, loaded, &result);
  g_main_loop_run (loop);
  g_assert_no_error (result.error);
  g_assert_nonnull (result.machines);
  g_assert_cmpuint (result.machines->len, >, 0);
  gboolean found = FALSE;
  for (guint i = 0; i < result.machines->len; i++)
    {
      PirwaMachine *machine = g_ptr_array_index (result.machines, i);
      g_assert_true (g_uuid_string_is_valid (pirwa_machine_get_uuid (machine)));
      if (g_str_equal (pirwa_machine_get_name (machine), "test"))
        {
          found = TRUE;
          g_assert_cmpstr (pirwa_machine_get_state (machine), ==, "Running");
        }
    }
  g_assert_true (found);
  g_ptr_array_unref (result.machines);
}

static void
test_cancelled (void)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  LoadResult result = { .loop = loop, .caller = g_thread_self () };
  g_cancellable_cancel (cancellable);
  pirwa_inventory_load_async ("test:///default", cancellable, loaded, &result);
  g_main_loop_run (loop);
  g_assert_null (result.machines);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&result.error);
}

static void
test_connection_failure (void)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  LoadResult result = { .loop = loop, .caller = g_thread_self () };
  pirwa_inventory_load_async ("pirwa-unsupported:///invalid", NULL, loaded, &result);
  g_main_loop_run (loop);
  g_assert_null (result.machines);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (result.error->message, -1, "Cannot connect to libvirt"));
  g_clear_error (&result.error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/inventory/discovery", test_discovery);
  g_test_add_func ("/inventory/cancelled", test_cancelled);
  g_test_add_func ("/inventory/connection-failure", test_connection_failure);
  return g_test_run ();
}
