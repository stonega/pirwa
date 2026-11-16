/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic media/capabilities around real test-driver provisioning. */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-provision.h"
#include <glib/gstdio.h>
#include <osinfo/osinfo.h>
#include <unistd.h>

static const char capabilities[]
    = "<domainCapabilities><cpu><mode name='host-passthrough' supported='yes'/></cpu>"
      "<os><enum name='firmware'><value>efi</value></enum></os><devices>"
      "<graphics><enum name='type'><value>spice</value></enum></graphics>"
      "<video><enum name='modelType'><value>qxl</value></enum></video>"
      "<tpm supported='yes'><enum name='model'><value>tpm-crb</value></enum>"
      "<enum name='backendModel'><value>emulator</value></enum>"
      "<enum name='backendVersion'><value>2.0</value></enum></tpm>"
      "</devices></domainCapabilities>";

static const char *caps;
static const char *architecture;
static OsinfoOs *detected_os;
static gboolean fail_start;
static gboolean fail_drivers;
static guint allocations;
static guint undefine_flags;
static guint media_reads;
static char *definition;

char *__wrap_virConnectGetDomainCapabilities (virConnectPtr connection, const char *emulator,
                                              const char *arch, const char *machine,
                                              const char *type, unsigned int flags);
OsinfoMedia *__wrap_osinfo_media_create_from_location_with_flags (const char *location,
                                                                  GCancellable *cancellable,
                                                                  guint flags, GError **error);
gboolean __wrap_osinfo_db_identify_media (OsinfoDb *db, OsinfoMedia *media);
OsinfoOs *__wrap_osinfo_media_get_os (OsinfoMedia *media);
virStorageVolPtr __real_virStorageVolCreateXML (virStoragePoolPtr pool, const char *xml,
                                                unsigned int flags);
virStorageVolPtr __wrap_virStorageVolCreateXML (virStoragePoolPtr pool, const char *xml,
                                                unsigned int flags);
int __real_virStorageVolDelete (virStorageVolPtr volume, unsigned int flags);
int __wrap_virStorageVolDelete (virStorageVolPtr volume, unsigned int flags);
virDomainPtr __real_virDomainDefineXML (virConnectPtr connection, const char *xml);
virDomainPtr __wrap_virDomainDefineXML (virConnectPtr connection, const char *xml);
int __real_virDomainUndefineFlags (virDomainPtr domain, unsigned int flags);
int __wrap_virDomainUndefineFlags (virDomainPtr domain, unsigned int flags);
gboolean __wrap_pirwa_network_start (virConnectPtr connection, virDomainPtr domain,
                                     GCancellable *cancellable, GError **error);

char *
__wrap_virConnectGetDomainCapabilities (virConnectPtr connection, const char *emulator,
                                        const char *arch, const char *machine, const char *type,
                                        unsigned int flags)
{
  (void)connection;
  (void)emulator;
  (void)flags;
  g_assert_cmpstr (arch, ==, "x86_64");
  g_assert_cmpstr (machine, ==, "q35");
  g_assert_cmpstr (type, ==, "kvm");
  return g_strdup (caps);
}

OsinfoMedia *
__wrap_osinfo_media_create_from_location_with_flags (const char *location,
                                                     GCancellable *cancellable, guint flags,
                                                     GError **error)
{
  (void)cancellable;
  media_reads++;
  gboolean driver = g_str_has_suffix (location, "drivers.iso");
  g_assert_cmpuint (flags, ==, driver ? 0 : OSINFO_MEDIA_DETECT_REQUIRE_BOOTABLE);
  if (driver && fail_drivers)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Invalid fixture driver ISO");
      return NULL;
    }
  return osinfo_media_new (location, architecture);
}

gboolean
__wrap_osinfo_db_identify_media (OsinfoDb *db, OsinfoMedia *media)
{
  (void)db;
  (void)media;
  return detected_os != NULL;
}

OsinfoOs *
__wrap_osinfo_media_get_os (OsinfoMedia *media)
{
  (void)media;
  return detected_os;
}

virStorageVolPtr
__wrap_virStorageVolCreateXML (virStoragePoolPtr pool, const char *xml, unsigned int flags)
{
  allocations++;
  virStorageVolPtr volume = __real_virStorageVolCreateXML (pool, xml, flags);
  if (volume)
    {
      /* The test driver owns metadata only. Supply an inode for the production
       * ownership/reference checks, without allocating a guest-sized disk. */
      g_autofree char *path = virStorageVolGetPath (volume);
      g_assert_true (g_file_set_contents (path, "fixture disk", -1, NULL));
    }
  return volume;
}

int
__wrap_virStorageVolDelete (virStorageVolPtr volume, unsigned int flags)
{
  g_autofree char *path = virStorageVolGetPath (volume);
  int result = __real_virStorageVolDelete (volume, flags);
  if (result == 0)
    g_unlink (path);
  return result;
}

virDomainPtr
__wrap_virDomainDefineXML (virConnectPtr connection, const char *xml)
{
  g_free (definition);
  definition = g_strdup (xml);
  /* Model QEMU's firmware normalization; the test driver creates no NVRAM. */
  g_autoptr (GString) normalized = g_string_new (xml);
  const char *end = strstr (xml, "</os>");
  g_assert_nonnull (end);
  g_string_insert (normalized, end - xml, "<nvram>/fixture/windows_VARS.fd</nvram>");
  return __real_virDomainDefineXML (connection, normalized->str);
}

int
__wrap_virDomainUndefineFlags (virDomainPtr domain, unsigned int flags)
{
  undefine_flags = flags;
  /* The synthetic driver owns no firmware or TPM files. Check the production
   * flags, then omit those unsupported flags from its adapter. */
  return __real_virDomainUndefineFlags (
      domain, flags
                  & ~(VIR_DOMAIN_UNDEFINE_TPM | VIR_DOMAIN_UNDEFINE_KEEP_TPM
                      | VIR_DOMAIN_UNDEFINE_NVRAM | VIR_DOMAIN_UNDEFINE_KEEP_NVRAM));
}

gboolean
__wrap_pirwa_network_start (virConnectPtr connection, virDomainPtr domain,
                            GCancellable *cancellable, GError **error)
{
  (void)connection;
  (void)cancellable;
  if (fail_start)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fixture start failure");
      return FALSE;
    }
  g_assert_cmpint (virDomainCreate (domain), ==, 0);
  return TRUE;
}

typedef struct
{
  virConnectPtr connection;
  virStoragePoolPtr pool;
  char *directory;
  PirwaCreate options;
} Fixture;

static void
setup (Fixture *fixture, gconstpointer data)
{
  (void)data;
  caps = capabilities;
  architecture = "x86_64";
  fail_start = fail_drivers = FALSE;
  allocations = undefine_flags = media_reads = 0;
  g_clear_pointer (&definition, g_free);
  detected_os = osinfo_os_new ("http://microsoft.com/win/11");
  fixture->directory = g_dir_make_tmp ("pirwa-windows-XXXXXX", NULL);
  g_assert_nonnull (fixture->directory);
  fixture->options
      = (PirwaCreate){ .name = "Windows fixture",
                       .pool = "windows-fixture",
                       .source = g_build_filename (fixture->directory, "windows.iso", NULL),
                       .driver_iso = g_build_filename (fixture->directory, "drivers.iso", NULL),
                       .cpus = 2,
                       .memory_mb = 4096,
                       .disk_gb = 64,
                       .guest = PIRWA_GUEST_WINDOWS_11 };
  g_assert_true (g_file_set_contents (fixture->options.source, "fixture media", -1, NULL));
  g_assert_true (g_file_set_contents (fixture->options.driver_iso, "fixture drivers", -1, NULL));
  fixture->connection = virConnectOpen ("test:///default");
  g_assert_nonnull (fixture->connection);
  g_autofree char *xml = g_strdup_printf (
      "<pool type='dir'><name>windows-fixture</name><target><path>%s</path></target></pool>",
      fixture->directory);
  fixture->pool = virStoragePoolDefineXML (fixture->connection, xml, 0);
  g_assert_nonnull (fixture->pool);
  g_assert_cmpint (virStoragePoolCreate (fixture->pool, 0), ==, 0);
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  (void)data;
  virStorageVolPtr *volumes = NULL;
  int count = virStoragePoolListAllVolumes (fixture->pool, &volumes, 0);
  g_assert_cmpint (count, >=, 0);
  for (int i = 0; i < count; i++)
    {
      virStorageVolDelete (volumes[i], 0);
      virStorageVolFree (volumes[i]);
    }
  free (volumes);
  virStoragePoolDestroy (fixture->pool);
  virStoragePoolUndefine (fixture->pool);
  virStoragePoolFree (fixture->pool);
  virConnectClose (fixture->connection);
  g_unlink (fixture->options.source);
  g_unlink (fixture->options.driver_iso);
  g_rmdir (fixture->directory);
  g_free (fixture->options.source);
  g_free (fixture->options.driver_iso);
  g_free (fixture->directory);
  g_clear_object (&detected_os);
  g_clear_pointer (&definition, g_free);
}

static void
test_create_remove (Fixture *fixture, gconstpointer data)
{
  gboolean delete_disk = GPOINTER_TO_INT (data);
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid
      = pirwa_provision_create (fixture->connection, &fixture->options, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (uuid);
  g_assert_cmpuint (allocations, ==, 1);
  g_assert_cmpuint (media_reads, ==, 2);
  g_assert_true (pirwa_config_supports (
      definition, "/domain/os[@firmware='efi']/firmware/feature[@name='enrolled-keys' and "
                  "@enabled='yes'] and /domain/devices/tpm/backend[@version='2.0'] and "
                  "count(/domain/devices/disk[@device='cdrom'])=2"));
  virDomainPtr domain = virDomainLookupByUUIDString (fixture->connection, uuid);
  g_assert_nonnull (domain);
  g_assert_cmpint (virDomainDestroy (domain), ==, 0);
  g_assert_true (
      pirwa_provision_remove (fixture->connection, domain, uuid, delete_disk, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (
      (undefine_flags & (delete_disk ? VIR_DOMAIN_UNDEFINE_TPM : VIR_DOMAIN_UNDEFINE_KEEP_TPM))
      != 0);
  g_assert_true (
      (undefine_flags & (delete_disk ? VIR_DOMAIN_UNDEFINE_NVRAM : VIR_DOMAIN_UNDEFINE_KEEP_NVRAM))
      != 0);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, delete_disk ? 0 : 1);
  virDomainFree (domain);
}

static void
test_start_rollback (Fixture *fixture, gconstpointer data)
{
  (void)data;
  fail_start = TRUE;
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid
      = pirwa_provision_create (fixture->connection, &fixture->options, NULL, NULL, NULL, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "Fixture start failure");
  g_assert_true ((undefine_flags & VIR_DOMAIN_UNDEFINE_TPM) != 0);
  g_assert_true ((undefine_flags & VIR_DOMAIN_UNDEFINE_NVRAM) != 0);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, 0);
}

static void
test_preflight (Fixture *fixture, gconstpointer data)
{
  switch (GPOINTER_TO_INT (data))
    {
    case 0:
      caps = "<domainCapabilities/>";
      break;
    case 1:
      architecture = "aarch64";
      break;
    case 2:
      g_clear_object (&detected_os);
      detected_os = osinfo_os_new ("http://microsoft.com/win/10");
      break;
    case 3:
      fixture->options.guest = PIRWA_GUEST_LINUX;
      break;
    case 4:
      fail_drivers = TRUE;
      break;
    case 5:
      g_assert_cmpint (g_unlink (fixture->options.driver_iso), ==, 0);
      g_assert_cmpint (symlink (fixture->options.source, fixture->options.driver_iso), ==, 0);
      break;
    }
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid
      = pirwa_provision_create (fixture->connection, &fixture->options, NULL, NULL, NULL, &error);
  g_assert_null (uuid);
  g_assert_nonnull (error);
  g_assert_cmpuint (allocations, ==, 0);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, 0);
  g_assert_null (definition);
}

int
main (int argc, char **argv)
{
  g_autofree char *state = g_dir_make_tmp ("pirwa-windows-state-XXXXXX", NULL);
  g_assert_nonnull (state);
  g_setenv ("XDG_DATA_HOME", state, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/windows/create-keep", Fixture, GINT_TO_POINTER (FALSE), setup, test_create_remove,
              teardown);
  g_test_add ("/windows/create-delete", Fixture, GINT_TO_POINTER (TRUE), setup, test_create_remove,
              teardown);
  g_test_add ("/windows/start-rollback", Fixture, NULL, setup, test_start_rollback, teardown);
  const char *names[]
      = { "host", "architecture", "profile", "linux-profile", "driver-iso", "symlink" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      g_autofree char *path = g_strdup_printf ("/windows/preflight/%s", names[i]);
      g_test_add (path, Fixture, GINT_TO_POINTER (i), setup, test_preflight, teardown);
    }
  int result = g_test_run ();
  /* Only the private ownership records can remain when keeping a disk. */
  g_autofree char *ownership = g_build_filename (state, "pirwa", "ownership", NULL);
  g_autoptr (GDir) directory = g_dir_open (ownership, 0, NULL);
  const char *name;
  while (directory && (name = g_dir_read_name (directory)))
    {
      g_autofree char *path = g_build_filename (ownership, name, NULL);
      g_autoptr (GDir) records = g_dir_open (path, 0, NULL);
      const char *record;
      while (records && (record = g_dir_read_name (records)))
        {
          g_autofree char *file = g_build_filename (path, record, NULL);
          g_unlink (file);
        }
      g_rmdir (path);
    }
  g_rmdir (ownership);
  g_autofree char *app = g_build_filename (state, "pirwa", NULL);
  g_rmdir (app);
  g_rmdir (state);
  return result;
}
