/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-provision.h"
#include <glib/gstdio.h>
#include <unistd.h>

typedef struct
{
  virConnectPtr connection;
  virStoragePoolPtr pool;
  virStorageVolPtr volume;
  virDomainPtr domain;
  char *directory;
  char *path;
} Fixture;

static void
setup (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  fixture->directory = g_dir_make_tmp ("pirwa-provision-XXXXXX", &error);
  g_assert_no_error (error);
  fixture->connection = virConnectOpen ("test:///default");
  g_assert_nonnull (fixture->connection);
  g_autofree char *pool_xml = g_strdup_printf (
      "<pool type='dir'><name>fixture-pool</name><target><path>%s</path></target></pool>",
      fixture->directory);
  fixture->pool = virStoragePoolDefineXML (fixture->connection, pool_xml, 0);
  g_assert_nonnull (fixture->pool);
  g_assert_cmpint (virStoragePoolCreate (fixture->pool, 0), ==, 0);
  g_autofree char *volume_xml
      = pirwa_config_volume ("11111111-1111-4111-8111-111111111111", 1ULL << 30);
  fixture->volume = virStorageVolCreateXML (fixture->pool, volume_xml, 0);
  g_assert_nonnull (fixture->volume);
  fixture->path = virStorageVolGetPath (fixture->volume);
  /* The test driver tracks metadata without creating files. Supply a real
   * temporary inode for alias checks, never a real guest disk. */
  g_assert_true (g_file_set_contents (fixture->path, "fixture", -1, &error));
  g_assert_no_error (error);
  g_autofree char *xml = g_strdup_printf (
      "<domain type='test'><name>fixture-vm</name><memory>131072</memory>"
      "<vcpu>1</vcpu><os><type>hvm</type></os><devices>"
      "<disk type='file' device='disk'><source file='%s'/><target dev='vda' bus='virtio'/>"
      "</disk></devices></domain>",
      fixture->path);
  fixture->domain = virDomainDefineXML (fixture->connection, xml);
  g_assert_nonnull (fixture->domain);
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  (void)data;
  if (fixture->domain)
    {
      if (virDomainIsActive (fixture->domain) == 1)
        virDomainDestroy (fixture->domain);
      virDomainUndefine (fixture->domain);
      virDomainFree (fixture->domain);
    }
  virStorageVolDelete (fixture->volume, 0);
  virStorageVolFree (fixture->volume);
  virStoragePoolDestroy (fixture->pool);
  virStoragePoolUndefine (fixture->pool);
  virStoragePoolFree (fixture->pool);
  virConnectClose (fixture->connection);
  g_unlink (fixture->path);
  g_rmdir (fixture->directory);
  g_free (fixture->path);
  g_free (fixture->directory);
}

static void
test_references (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autofree char *missing = g_build_filename (fixture->directory, "missing.iso", NULL);
  g_autofree char *unrelated = g_strdup_printf (
      "<domain><devices><disk device='cdrom'><source file='%s'/></disk></devices></domain>",
      missing);
  g_assert_false (pirwa_provision_references (unrelated, fixture->path, "owned.qcow2"));
  g_autofree char *reference = g_strdup_printf (
      "<domain><devices><disk><source file='%s'/></disk></devices></domain>", fixture->path);
  g_assert_true (pirwa_provision_references (reference, fixture->path, "owned.qcow2"));
  g_autofree char *alias = g_build_filename (fixture->directory, "alias", NULL);
  g_assert_cmpint (symlink (fixture->path, alias), ==, 0);
  g_autofree char *aliased
      = g_strdup_printf ("<domain><devices><disk><backingStore><source "
                         "file='%s'/></backingStore></disk></devices></domain>",
                         alias);
  g_assert_true (pirwa_provision_references (aliased, fixture->path, "owned.qcow2"));
  g_unlink (alias);
  g_assert_cmpint (link (fixture->path, alias), ==, 0);
  g_assert_true (pirwa_provision_references (aliased, fixture->path, "owned.qcow2"));
  g_unlink (alias);
  g_assert_true (pirwa_provision_references ("<invalid>", fixture->path, "owned.qcow2"));
  g_assert_true (pirwa_provision_references (NULL, fixture->path, "owned.qcow2"));
  g_assert_true (pirwa_provision_references (
      "<!DOCTYPE domain [<!ENTITY file '/tmp/unknown'>]><domain/>", fixture->path, "owned.qcow2"));
}

static void
test_rollback_inactive (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_assert_true (
      pirwa_provision_rollback (fixture->connection, fixture->domain, fixture->volume, &error));
  g_assert_no_error (error);
  virDomainFree (fixture->domain);
  fixture->domain = NULL;
  virStorageVolPtr missing = virStorageVolLookupByName (
      fixture->pool, "pirwa-11111111-1111-4111-8111-111111111111.qcow2");
  g_assert_null (missing);
}

static void
test_rollback_running (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_assert_cmpint (virDomainCreate (fixture->domain), ==, 0);
  g_assert_false (
      pirwa_provision_rollback (fixture->connection, fixture->domain, fixture->volume, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_cmpint (virDomainIsActive (fixture->domain), ==, 1);
  g_assert_cmpint (virDomainIsPersistent (fixture->domain), ==, 1);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, 1);
}

static void
test_rollback_shared (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_autofree char *xml = g_strdup_printf (
      "<domain type='test'><name>other-fixture</name><memory>131072</memory>"
      "<vcpu>1</vcpu><os><type>hvm</type></os><devices>"
      "<disk type='file' device='disk'><source file='%s'/><target dev='vda' bus='virtio'/>"
      "</disk></devices></domain>",
      fixture->path);
  virDomainPtr other = virDomainDefineXML (fixture->connection, xml);
  g_assert_nonnull (other);
  g_assert_false (
      pirwa_provision_rollback (fixture->connection, fixture->domain, fixture->volume, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_cmpint (virDomainIsPersistent (fixture->domain), ==, 1);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, 1);
  virDomainUndefine (other);
  virDomainFree (other);
}

static void
test_cancel_remove (Fixture *fixture, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  char uuid[VIR_UUID_STRING_BUFLEN];
  g_assert_cmpint (virDomainGetUUIDString (fixture->domain, uuid), ==, 0);
  g_assert_false (
      pirwa_provision_remove (fixture->connection, fixture->domain, uuid, FALSE, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (virDomainIsPersistent (fixture->domain), ==, 1);
  g_assert_cmpint (virStoragePoolNumOfVolumes (fixture->pool), ==, 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/provision/references-and-missing-media", Fixture, NULL, setup, test_references,
              teardown);
  g_test_add ("/provision/rollback-inactive", Fixture, NULL, setup, test_rollback_inactive,
              teardown);
  g_test_add ("/provision/rollback-running", Fixture, NULL, setup, test_rollback_running, teardown);
  g_test_add ("/provision/rollback-shared", Fixture, NULL, setup, test_rollback_shared, teardown);
  g_test_add ("/provision/cancel-before-remove", Fixture, NULL, setup, test_cancel_remove,
              teardown);
  return g_test_run ();
}
