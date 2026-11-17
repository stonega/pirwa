/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-import.h"
#include "pirwa-provision.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *source_uuid = "11111111-1111-4111-8111-111111111111";
static char *test_data;
static virConnectPtr shared_source;
static gboolean fail_send;

/* Test-driver connections do not share power state. Reuse the fixture handle
 * with a real reference to model two connections to one libvirt service. */
virConnectPtr
__wrap_virConnectOpenReadOnly (const char *uri)
{
  (void)uri;
  g_assert_nonnull (shared_source);
  g_assert_cmpint (virConnectRef (shared_source), ==, 0);
  return shared_source;
}

typedef struct
{
  char *root;
  char *disk;
  char *loader;
  char *nvram;
  char *xml;
  char *source_uri;
  virConnectPtr source;
  virConnectPtr destination;
  virStoragePoolPtr pool;
  virDomainPtr original;
} Fixture;

/* The test driver implements definitions and storage metadata, but no upload
 * transport or disk files. Emulate only that transport; use real temporary
 * files and the real libvirt test driver for the rest of the import. */
typedef struct
{
  int fd;
} Upload;

virStreamPtr
__wrap_virStreamNew (virConnectPtr connection, unsigned int flags)
{
  (void)connection;
  (void)flags;
  Upload *upload = g_new0 (Upload, 1);
  upload->fd = -1;
  return (virStreamPtr)upload;
}

int
__wrap_virStorageVolUpload (virStorageVolPtr volume, virStreamPtr stream, unsigned long long offset,
                            unsigned long long length, unsigned int flags)
{
  (void)offset;
  (void)length;
  (void)flags;
  g_autofree char *path = virStorageVolGetPath (volume);
  Upload *upload = (Upload *)stream;
  upload->fd = g_open (path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  return upload->fd < 0 ? -1 : 0;
}

int
__wrap_virStreamSend (virStreamPtr stream, const char *data, size_t count)
{
  return fail_send ? -1 : write (((Upload *)stream)->fd, data, count);
}

int
__wrap_virStreamFinish (virStreamPtr stream)
{
  return fsync (((Upload *)stream)->fd);
}

int
__wrap_virStreamAbort (virStreamPtr stream)
{
  (void)stream;
  return 0;
}

int
__wrap_virStreamFree (virStreamPtr stream)
{
  Upload *upload = (Upload *)stream;
  if (upload->fd >= 0)
    close (upload->fd);
  g_free (upload);
  return 0;
}

int __real_virStorageVolDelete (virStorageVolPtr volume, unsigned int flags);
int
__wrap_virStorageVolDelete (virStorageVolPtr volume, unsigned int flags)
{
  g_autofree char *path = virStorageVolGetPath (volume);
  int status = __real_virStorageVolDelete (volume, flags);
  if (status == 0)
    g_unlink (path);
  return status;
}

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
  const char *name;
  if (!dir)
    {
      g_unlink (path);
      return;
    }
  while ((name = g_dir_read_name (dir)))
    {
      g_autofree char *child = g_build_filename (path, name, NULL);
      remove_tree (child);
    }
  g_rmdir (path);
}

static void
setup (Fixture *f, gconstpointer data)
{
  (void)data;
  fail_send = FALSE;
  g_autoptr (GError) error = NULL;
  f->root = g_dir_make_tmp ("pirwa-import-XXXXXX", &error);
  g_assert_no_error (error);
  f->disk = g_build_filename (f->root, "source.qcow2", NULL);
  f->loader = g_build_filename (f->root, "loader.fd", NULL);
  f->nvram = g_build_filename (f->root, "nvram.fd", NULL);
  guint8 header[104] = { 'Q', 'F', 'I', 0xfb };
  guint32 version = GUINT32_TO_BE (3), cluster = GUINT32_TO_BE (16);
  guint64 capacity = GUINT64_TO_BE (1ULL << 30);
  memcpy (header + 4, &version, 4);
  memcpy (header + 20, &cluster, 4);
  memcpy (header + 24, &capacity, 8);
  g_assert_true (g_file_set_contents (f->disk, (char *)header, sizeof header, &error));
  g_assert_true (g_file_set_contents (f->loader, "firmware bytes", -1, &error));
  g_assert_true (g_file_set_contents (f->nvram, "guest boot entries and keys", -1, &error));
  g_assert_no_error (error);
  f->xml = g_strdup_printf (
      "<domain type='test'><name>Boxes Example</name><uuid>%s</uuid>"
      "<memory unit='KiB'>524288</memory><vcpu>2</vcpu>"
      "<metadata><o:libosinfo xmlns:o='%s'><o:os id='http://fedoraproject.org/fedora/43'/>"
      "</o:libosinfo></metadata>"
      "<os><type arch='x86_64'>hvm</type><loader type='pflash' readonly='yes'>%s</loader>"
      "<nvram template='/app/share/template.fd'>%s</nvram></os>"
      "<features><acpi/></features><devices><emulator>/app/bin/qemu-system-x86_64</emulator>"
      "<disk type='file' device='disk'><driver name='qemu' type='qcow2'/>"
      "<source file='%s'/><target dev='vda' bus='virtio'/></disk>"
      "<disk type='file' device='cdrom'><source file='/missing/install.iso'/>"
      "<target dev='sda' bus='sata'/></disk>"
      "<interface type='user'><mac address='52:54:00:11:22:33'/><model type='virtio'/></interface>"
      "<graphics type='spice' socket='/run/user/999/boxes.sock'/></devices></domain>",
      source_uuid, PIRWA_OSINFO_NS, f->loader, f->nvram, f->disk);
  g_autofree char *node_path = g_build_filename (f->root, "source.xml", NULL);
  g_autofree char *node = g_strdup_printf ("<node>%s</node>", f->xml);
  g_assert_true (g_file_set_contents (node_path, node, -1, &error));
  f->source_uri = g_strconcat ("test://", node_path, NULL);
  f->source = virConnectOpen (f->source_uri);
  g_assert_nonnull (f->source);
  shared_source = f->source;
  f->original = virDomainLookupByUUIDString (f->source, source_uuid);
  g_assert_nonnull (f->original);
  /* File-driver fixtures start active. Stop only this disposable synthetic guest. */
  g_assert_cmpint (virDomainDestroy (f->original), ==, 0);
  f->destination = virConnectOpen ("test:///default");
  g_assert_nonnull (f->destination);
  g_autofree char *pool_xml = g_strdup_printf (
      "<pool type='dir'><name>pirwa</name><target><path>%s</path></target></pool>", f->root);
  f->pool = virStoragePoolDefineXML (f->destination, pool_xml, 0);
  g_assert_nonnull (f->pool);
  g_assert_cmpint (virStoragePoolCreate (f->pool, 0), ==, 0);
}

static void
teardown (Fixture *f, gconstpointer data)
{
  (void)data;
  virDomainFree (f->original);
  virConnectClose (f->source);
  shared_source = NULL;
  virStoragePoolDestroy (f->pool);
  virStoragePoolUndefine (f->pool);
  virStoragePoolFree (f->pool);
  virConnectClose (f->destination);
  remove_tree (f->root);
  g_free (f->root);
  g_free (f->disk);
  g_free (f->loader);
  g_free (f->nvram);
  g_free (f->xml);
  g_free (f->source_uri);
}

static void
test_copy (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_autofree char *before = virDomainGetXMLDesc (f->original, VIR_DOMAIN_XML_INACTIVE);
  g_autofree char *uuid = pirwa_provision_import_boxes (f->destination, f->source_uri, source_uuid,
                                                        NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (uuid);
  g_assert_cmpstr (uuid, !=, source_uuid);
  virDomainPtr copy = virDomainLookupByUUIDString (f->destination, uuid);
  g_assert_nonnull (copy);
  g_autofree char *internal_name = g_strdup_printf ("pirwa-%s", uuid);
  g_assert_cmpstr (virDomainGetName (copy), ==, internal_name);
  g_assert_cmpint (virDomainIsActive (copy), ==, 0);
  g_autofree char *xml = virDomainGetXMLDesc (copy, VIR_DOMAIN_XML_INACTIVE);
  g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, &error);
  g_assert_no_error (error);
  g_assert_true (pirwa_provision_owned (f->destination, uuid, details));
  g_autofree char *display_name = g_strdup_printf ("Boxes Example (Imported %.8s)", uuid);
  g_assert_cmpstr (details->name, ==, display_name);
  g_assert_cmpuint (details->cpus, ==, 2);
  g_assert_cmpuint (details->memory_mb, ==, 512);
  g_assert_cmpstr (details->disk_path, !=, f->disk);
  g_autofree char *id = pirwa_config_os_id (xml);
  g_assert_cmpstr (id, ==, "http://fedoraproject.org/fedora/43");
  g_assert_null (strstr (xml, "/app/"));
  g_assert_null (strstr (xml, "/missing/install.iso"));
  g_assert_null (strstr (xml, "52:54:00:11:22:33"));
  g_assert_null (strstr (xml, "boxes.sock"));
  g_autofree char *after = virDomainGetXMLDesc (f->original, VIR_DOMAIN_XML_INACTIVE);
  g_assert_cmpstr (before, ==, after);
  g_assert_cmpint (virDomainIsActive (f->original), ==, 0);
  g_autofree char *source_bytes = NULL, *copy_bytes = NULL;
  gsize source_size, copy_size;
  g_assert_true (g_file_get_contents (f->disk, &source_bytes, &source_size, &error));
  g_assert_true (g_file_get_contents (details->disk_path, &copy_bytes, &copy_size, &error));
  g_assert_cmpmem (source_bytes, source_size, copy_bytes, copy_size);
  /* The original can disappear without breaking the copied disk and firmware. */
  g_autofree char *firmware_root = g_build_filename (test_data, "pirwa", "firmware", NULL);
  g_autoptr (GDir) directory = g_dir_open (firmware_root, 0, &error);
  g_assert_no_error (error);
  const char *name = g_dir_read_name (directory);
  g_assert_nonnull (name);
  g_autofree char *vars = g_build_filename (firmware_root, name, "nvram.fd", NULL);
  g_autofree char *bytes = NULL;
  g_assert_true (g_file_get_contents (vars, &bytes, NULL, &error));
  g_assert_cmpstr (bytes, ==, "guest boot entries and keys");
  g_assert_nonnull (strstr (xml, vars));
  struct stat st;
  g_assert_cmpint (g_stat (vars, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  /* NVRAM undefine flags are unsupported by the synthetic driver. Ownership
   * is verified above; production removal remains covered by provision/KVM QA. */
  g_assert_cmpint (virDomainUndefine (copy), ==, 0);
  virDomainFree (copy);
}

static void
test_running (Fixture *f, gconstpointer data)
{
  (void)data;
  g_assert_cmpint (virDomainCreate (f->original), ==, 0);
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = pirwa_provision_import_boxes (f->destination, f->source_uri, source_uuid,
                                                        NULL, NULL, NULL, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_cmpint (virStoragePoolNumOfVolumes (f->pool), ==, 0);
  g_assert_cmpint (virDomainIsActive (f->original), ==, 1);
}

static void
cancel_copy (double fraction, const char *stage, gpointer data)
{
  (void)stage;
  if (fraction > 0.15)
    g_cancellable_cancel (data);
}

static void
test_cancel (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *before = virDomainGetXMLDesc (f->original, VIR_DOMAIN_XML_INACTIVE);
  g_autofree char *uuid = pirwa_provision_import_boxes (
      f->destination, f->source_uri, source_uuid, cancellable, cancel_copy, cancellable, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (virStoragePoolNumOfVolumes (f->pool), ==, 0);
  g_autofree char *after = virDomainGetXMLDesc (f->original, VIR_DOMAIN_XML_INACTIVE);
  g_assert_cmpstr (before, ==, after);
}

static guint
firmware_count (void)
{
  g_autofree char *root = g_build_filename (test_data, "pirwa", "firmware", NULL);
  g_autoptr (GDir) directory = g_dir_open (root, 0, NULL);
  guint count = 0;
  while (directory && g_dir_read_name (directory))
    count++;
  return count;
}

static void
change_source (double fraction, const char *stage, gpointer data)
{
  (void)stage;
  if (fraction > 0.15)
    g_assert_true (g_file_set_contents (data, "changed source", -1, NULL));
}

static void
test_source_changed (Fixture *f, gconstpointer data)
{
  (void)data;
  guint before = firmware_count ();
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = pirwa_provision_import_boxes (f->destination, f->source_uri, source_uuid,
                                                        NULL, change_source, f->disk, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_cmpint (virStoragePoolNumOfVolumes (f->pool), ==, 0);
  g_assert_cmpuint (firmware_count (), ==, before);
}

static void
test_upload_failed (Fixture *f, gconstpointer data)
{
  (void)data;
  guint before = firmware_count ();
  fail_send = TRUE;
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = pirwa_provision_import_boxes (f->destination, f->source_uri, source_uuid,
                                                        NULL, NULL, NULL, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpint (virStoragePoolNumOfVolumes (f->pool), ==, 0);
  g_assert_cmpuint (firmware_count (), ==, before);
  g_assert_cmpint (virDomainIsActive (f->original), ==, 0);
}

static void
test_snapshots (Fixture *f, gconstpointer data)
{
  (void)data;
  int fd = g_open (f->disk, O_RDWR, 0);
  g_assert_cmpint (fd, >=, 0);
  guint32 count = GUINT32_TO_BE (1);
  g_assert_cmpint (pwrite (fd, &count, sizeof count, 60), ==, sizeof count);
  close (fd);
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = pirwa_provision_import_boxes (f->destination, f->source_uri, source_uuid,
                                                        NULL, NULL, NULL, &error);
  g_assert_null (uuid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpint (virStoragePoolNumOfVolumes (f->pool), ==, 0);
}

static void
test_reject (Fixture *f, gconstpointer data)
{
  (void)data;
  const char *devices[]
      = { "<tpm model='tpm-crb'><backend type='emulator'/></tpm>", "<filesystem type='mount'/>",
          "<hostdev type='pci'/>",
          "<disk type='file' device='disk'><driver type='qcow2'/><source file='/extra'/></disk>" };
  for (guint i = 0; i < G_N_ELEMENTS (devices); i++)
    {
      g_auto (GStrv) parts = g_strsplit (f->xml, "</devices>", 2);
      g_autofree char *xml = g_strconcat (parts[0], devices[i], "</devices>", parts[1], NULL);
      g_autoptr (GError) error = NULL;
      g_autoptr (PirwaImport) plan = pirwa_import_prepare (xml, &error);
      g_assert_null (plan);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    }
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaImport) plan = pirwa_import_prepare (
      "<!DOCTYPE domain [<!ENTITY x SYSTEM 'file:///etc/hostname'>]><domain/>", &error);
  g_assert_null (plan);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_firmware (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *alias = g_build_filename (f->root, "alias", NULL);
  g_autofree char *target = g_build_filename (f->root, "target", NULL);
  g_assert_cmpint (symlink (f->loader, alias), ==, 0);
  g_autoptr (GError) error = NULL;
  g_assert_false (pirwa_import_copy_firmware (alias, target, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_true (pirwa_import_copy_firmware (f->loader, target, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (pirwa_import_copy_firmware (f->nvram, target, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_autofree char *bytes = NULL;
  g_assert_true (g_file_get_contents (target, &bytes, NULL, NULL));
  g_assert_cmpstr (bytes, ==, "firmware bytes");
}

int
main (int argc, char **argv)
{
  test_data = g_dir_make_tmp ("pirwa-import-data-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", test_data, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/import/copy-independent-machine", Fixture, NULL, setup, test_copy, teardown);
  g_test_add ("/import/reject-running", Fixture, NULL, setup, test_running, teardown);
  g_test_add ("/import/cancel-and-rollback", Fixture, NULL, setup, test_cancel, teardown);
  g_test_add ("/import/reject-changed-source", Fixture, NULL, setup, test_source_changed, teardown);
  g_test_add ("/import/upload-failure-rolls-back", Fixture, NULL, setup, test_upload_failed,
              teardown);
  g_test_add ("/import/reject-internal-snapshots", Fixture, NULL, setup, test_snapshots, teardown);
  g_test_add ("/import/reject-unsupported-devices", Fixture, NULL, setup, test_reject, teardown);
  g_test_add ("/import/firmware-no-overwrite-or-symlink", Fixture, NULL, setup, test_firmware,
              teardown);
  int status = g_test_run ();
  remove_tree (test_data);
  g_free (test_data);
  return status;
}
