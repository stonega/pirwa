/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-config.h"
#include "pirwa-inventory.h"
#include "pirwa-libvirt.h"
#include <glib/gstdio.h>

static guint title_reads;
char *__real_virDomainGetMetadata (virDomainPtr domain, int type, const char *uri,
                                   unsigned int flags);
char *__wrap_virDomainGetMetadata (virDomainPtr domain, int type, const char *uri,
                                   unsigned int flags);

char *
__wrap_virDomainGetMetadata (virDomainPtr domain, int type, const char *uri, unsigned int flags)
{
  if (type == VIR_DOMAIN_METADATA_TITLE)
    title_reads++;
  return __real_virDomainGetMetadata (domain, type, uri, flags);
}

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
test_display_names (void)
{
  virConnectPtr connection = virConnectOpen ("test:///default");
  g_assert_nonnull (connection);
  virDomainPtr domains[2];
  const char *uuids[]
      = { "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222" };
  for (guint i = 0; i < G_N_ELEMENTS (domains); i++)
    {
      g_autofree char *xml = g_strdup_printf (
          "<domain type='test'><name>pirwa-%s</name><uuid>%s</uuid><title>test</title>"
          "<memory unit='MiB'>512</memory><vcpu>1</vcpu><os><type>hvm</type></os></domain>",
          uuids[i], uuids[i]);
      domains[i] = virDomainDefineXML (connection, xml);
      g_assert_nonnull (domains[i]);
    }
  PirwaInventoryCache *cache = pirwa_inventory_cache_new ();
  g_autoptr (GError) error = NULL;
  title_reads = 0;
  for (guint round = 0; round < 2; round++)
    {
      g_autoptr (GPtrArray) machines
          = pirwa_inventory_read_cached (connection, cache, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpuint (machines->len, ==, 3);
      g_assert_cmpuint (title_reads, ==, 3);
      for (guint i = 0; i < machines->len; i++)
        {
          PirwaMachine *machine = g_ptr_array_index (machines, i);
          g_assert_cmpstr (pirwa_machine_get_name (machine), ==, "test");
          if (i)
            g_assert_cmpstr (pirwa_machine_get_uuid (g_ptr_array_index (machines, i - 1)), <,
                             pirwa_machine_get_uuid (machine));
        }
    }
  g_assert_cmpint (
      virDomainSetMetadata (domains[0], VIR_DOMAIN_METADATA_TITLE, "Renamed & <VM>", NULL, NULL, 0),
      ==, 0);
  pirwa_inventory_cache_clear (cache);
  for (guint round = 0; round < 2; round++)
    {
      g_autoptr (GPtrArray) machines
          = pirwa_inventory_read_cached (connection, round ? NULL : cache, NULL, &error);
      g_assert_no_error (error);
      gboolean found = FALSE;
      for (guint i = 0; i < machines->len; i++)
        {
          PirwaMachine *machine = g_ptr_array_index (machines, i);
          if (g_str_equal (pirwa_machine_get_uuid (machine), uuids[0]))
            {
              found = TRUE;
              g_assert_cmpstr (pirwa_machine_get_name (machine), ==, "Renamed & <VM>");
            }
        }
      g_assert_true (found);
    }
  for (guint i = 0; i < G_N_ELEMENTS (domains); i++)
    {
      g_assert_cmpint (virDomainUndefine (domains[i]), ==, 0);
      virDomainFree (domains[i]);
    }
  pirwa_inventory_cache_free (cache);
  virConnectClose (connection);
}

static void
test_os_names (void)
{
  virConnectPtr connection = virConnectOpen ("test:///default");
  g_assert_nonnull (connection);
  virDomainPtr domain = virDomainLookupByName (connection, "test");
  g_assert_nonnull (domain);
  PirwaInventoryCache *cache = pirwa_inventory_cache_new ();
  g_autoptr (GError) error = NULL;
  const char *metadata[]
      = { "<libosinfo xmlns='" PIRWA_OSINFO_NS "'><os id='http://pirwa.example/os/1'/></libosinfo>",
          "<libosinfo xmlns='" PIRWA_OSINFO_NS
          "'><os id='http://pirwa.example/os/2'/></libosinfo>" };
  for (guint i = 0; i < G_N_ELEMENTS (metadata); i++)
    {
      g_assert_cmpint (virDomainSetMetadata (domain, VIR_DOMAIN_METADATA_ELEMENT, metadata[i],
                                             "libosinfo", PIRWA_OSINFO_NS, 0),
                       ==, 0);
      if (i)
        {
          g_autoptr (GPtrArray) cached
              = pirwa_inventory_read_cached (connection, cache, NULL, &error);
          g_assert_no_error (error);
          g_assert_cmpstr (pirwa_machine_get_title (g_ptr_array_index (cached, 0)), ==,
                           "Example Linux 1");
          pirwa_inventory_cache_clear (cache);
        }
      g_autoptr (GPtrArray) machines
          = pirwa_inventory_read_cached (connection, cache, NULL, &error);
      g_assert_no_error (error);
      PirwaMachine *machine = g_ptr_array_index (machines, 0);
      g_assert_cmpstr (pirwa_machine_get_title (machine), ==,
                       i ? "Example Linux 2" : "Example Linux 1");
      g_assert_cmpstr (pirwa_machine_get_name (machine), ==, "test");
    }
  g_assert_cmpint (
      virDomainSetMetadata (domain, VIR_DOMAIN_METADATA_ELEMENT, NULL, NULL, PIRWA_OSINFO_NS, 0),
      ==, 0);
  pirwa_inventory_cache_clear (cache);
  g_autoptr (GPtrArray) missing = pirwa_inventory_read_cached (connection, cache, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (pirwa_machine_get_title (g_ptr_array_index (missing, 0)), ==, "test");
  pirwa_inventory_cache_free (cache);
  virDomainFree (domain);
  virConnectClose (connection);
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
  g_autoptr (GError) error = NULL;
  g_autofree char *database = g_dir_make_tmp ("pirwa-osinfo-XXXXXX", &error);
  g_assert_no_error (error);
  g_autofree char *os_dir = g_build_filename (database, "os", "pirwa.example", NULL);
  g_assert_cmpint (g_mkdir_with_parents (os_dir, 0700), ==, 0);
  g_autofree char *file = g_build_filename (os_dir, "os-1.xml", NULL);
  g_assert_true (g_file_set_contents (
      file,
      "<libosinfo version='0.0.1'>"
      "<os id='http://pirwa.example/os/1'><short-id>example1</short-id><name>Example Linux "
      "1</name></os>"
      "</libosinfo>",
      -1, &error));
  g_assert_no_error (error);
  g_autofree char *second_file = g_build_filename (os_dir, "os-2.xml", NULL);
  g_assert_true (g_file_set_contents (
      second_file,
      "<libosinfo version='0.0.1'>"
      "<os id='http://pirwa.example/os/2'><short-id>example2</short-id><name>Example Linux "
      "2</name></os>"
      "</libosinfo>",
      -1, &error));
  g_assert_no_error (error);
  g_autofree char *empty = g_build_filename (database, "empty", NULL);
  g_assert_cmpint (g_mkdir (empty, 0700), ==, 0);
  g_setenv ("OSINFO_SYSTEM_DIR", database, TRUE);
  g_setenv ("OSINFO_LOCAL_DIR", empty, TRUE);
  g_setenv ("OSINFO_USER_DIR", empty, TRUE);
  g_test_add_func ("/inventory/os-names-and-cache", test_os_names);
  g_test_add_func ("/inventory/display-names-and-cache", test_display_names);
  g_test_add_func ("/inventory/discovery", test_discovery);
  g_test_add_func ("/inventory/cancelled", test_cancelled);
  g_test_add_func ("/inventory/connection-failure", test_connection_failure);
  int result = g_test_run ();
  g_unlink (file);
  g_unlink (second_file);
  g_rmdir (os_dir);
  g_autofree char *parent = g_path_get_dirname (os_dir);
  g_rmdir (parent);
  g_rmdir (empty);
  g_rmdir (database);
  return result;
}
