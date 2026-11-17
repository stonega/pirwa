/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in integration test: creates only uniquely named task-owned resources. */
#include "pirwa-backend.h"
#include "pirwa-libvirt.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  GMainLoop *loop;
  PirwaReply *reply;
  GError *error;
} Result;

static void
completed (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  r->reply = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &r->error);
  g_main_loop_quit (r->loop);
}

static PirwaReply *
request (PirwaBackend *backend, const PirwaRequest *operation, GError **error)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Result result = { .loop = loop };
  pirwa_backend_request_async (backend, operation, NULL, completed, &result);
  g_main_loop_run (loop);
  if (result.error)
    g_propagate_error (error, result.error);
  return result.reply;
}

static gboolean
command (PirwaBackend *backend, PirwaOperation operation, const char *uuid, GError **error)
{
  PirwaRequest op = { .operation = operation, .uuid = uuid };
  g_autoptr (PirwaReply) reply = request (backend, &op, error);
  return reply != NULL;
}

static void
cleanup_domain (virConnectPtr connection, const char *uuid)
{
  if (!uuid)
    return;
  virDomainPtr domain = virDomainLookupByUUIDString (connection, uuid);
  if (!domain)
    return;
  if (virDomainIsActive (domain) == 1)
    virDomainDestroy (domain);
  virDomainUndefineFlags (domain, VIR_DOMAIN_UNDEFINE_NVRAM);
  virDomainFree (domain);
}

int
main (void)
{
  if (g_strcmp0 (g_getenv ("PIRWA_RUN_KVM_TESTS"), "1") != 0)
    {
      g_printerr ("Set PIRWA_RUN_KVM_TESTS=1 to run isolated real-KVM tests.\n");
      return 77;
    }
  const char *iso = g_getenv ("PIRWA_TEST_ISO");
  if (!iso)
    iso = "/usr/share/edk2/ovmf/UefiShell.iso";
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp ("pirwa-kvm-XXXXXX", &error);
  if (!directory)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  g_autofree char *id = g_uuid_string_random ();
  g_autofree char *name = g_strconcat ("pirwa-integration-", id, NULL);
  g_autofree char *escaped = g_markup_escape_text (directory, -1);
  g_autofree char *pool_xml = g_strdup_printf (
      "<pool type='dir'><name>%s</name><target><path>%s</path></target></pool>", name, escaped);
  g_autofree char *created_uuid = NULL;
  g_autofree char *duplicate_uuid = NULL;
  g_autofree char *imported_uuid = NULL;
  g_autofree char *shared_uuid = NULL;
  g_autofree char *alias = g_build_filename (directory, "alias.qcow2", NULL);
  g_autofree char *image = g_build_filename (directory, "original.qcow2", NULL);
  g_autofree char *backing = g_build_filename (directory, "backed.qcow2", NULL);
  g_autofree char *source_data = NULL;
  g_autoptr (PirwaBackend) backend = NULL;
  virConnectPtr connection = virConnectOpen ("qemu:///session");
  virStoragePoolPtr pool = NULL;
  gboolean ok = FALSE;
  if (!connection)
    goto cleanup;
  pool = virStoragePoolDefineXML (connection, pool_xml, 0);
  if (!pool || virStoragePoolCreate (pool, 0) < 0)
    goto cleanup;
  backend = pirwa_backend_new ("qemu:///session");
  PirwaRequest op = { .operation = PIRWA_CREATE,
                      .options = { .name = name,
                                   .source = (char *)iso,
                                   .pool = name,
                                   .cpus = 1,
                                   .memory_mb = 512,
                                   .disk_gb = 4 } };
  {
    g_autoptr (PirwaReply) reply = request (backend, &op, &error);
    if (!reply)
      goto cleanup;
    created_uuid = g_strdup (reply->uuid);
  }
  g_print ("PASS: real KVM create, firmware boot, and SPICE configuration\n");
  /* The same display name creates another independent machine. */
  {
    g_autoptr (PirwaReply) duplicate = request (backend, &op, &error);
    if (!duplicate)
      goto cleanup;
    duplicate_uuid = g_strdup (duplicate->uuid);
    if (g_strcmp0 (duplicate_uuid, created_uuid) == 0 || virStoragePoolNumOfVolumes (pool) != 2
        || !command (backend, PIRWA_FORCE_STOP, duplicate_uuid, &error))
      goto cleanup;
    PirwaRequest remove
        = { .operation = PIRWA_REMOVE, .uuid = duplicate_uuid, .delete_disk = TRUE };
    g_autoptr (PirwaReply) removed = request (backend, &remove, &error);
    if (!removed || virStoragePoolNumOfVolumes (pool) != 1)
      {
        if (!error)
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Removing one same-name machine affected the other's storage");
        goto cleanup;
      }
    g_clear_pointer (&duplicate_uuid, g_free);
  }
  g_print ("PASS: duplicate display names retain independent identities and storage\n");
  if (!command (backend, PIRWA_PAUSE, created_uuid, &error)
      || !command (backend, PIRWA_RESUME, created_uuid, &error)
      || !command (backend, PIRWA_FORCE_STOP, created_uuid, &error))
    goto cleanup;
  {
    PirwaRequest detail = { .operation = PIRWA_DETAILS, .uuid = created_uuid };
    g_autoptr (PirwaReply) info = request (backend, &detail, &error);
    if (!info)
      goto cleanup;
    g_autoptr (GFile) link = g_file_new_for_path (alias);
    if (!g_file_make_symbolic_link (link, info->details->disk_path, NULL, &error))
      goto cleanup;
    shared_uuid = g_uuid_string_random ();
    g_autofree char *shared_name = g_strconcat (name, "-shared", NULL);
    PirwaCreate shared_options = op.options;
    shared_options.name = shared_name;
    g_autofree char *shared_xml
        = pirwa_config_domain (&shared_options, shared_uuid, alias, info->details->pool_uuid,
                               info->details->volume_key, TRUE, FALSE, NULL);
    virDomainPtr shared = virDomainDefineXML (connection, shared_xml);
    if (!shared)
      goto cleanup;
    virDomainFree (shared);
    PirwaRequest remove = { .operation = PIRWA_REMOVE, .uuid = created_uuid, .delete_disk = TRUE };
    g_autoptr (PirwaReply) rejected = request (backend, &remove, &error);
    if (rejected || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY))
      goto cleanup;
    g_clear_error (&error);
    cleanup_domain (connection, shared_uuid);
    g_clear_pointer (&shared_uuid, g_free);
    g_unlink (alias);
  }
  g_print ("PASS: shared-disk protection detects a symbolic-link alias\n");
  {
    g_autofree char *renamed = g_strconcat (name, "-edited", NULL);
    PirwaRequest edit = { .operation = PIRWA_EDIT,
                          .uuid = created_uuid,
                          .eject = TRUE,
                          .options = { .name = renamed, .cpus = 2, .memory_mb = 768 } };
    g_autoptr (PirwaReply) changed = request (backend, &edit, &error);
    if (!changed)
      goto cleanup;
    PirwaRequest detail = { .operation = PIRWA_DETAILS, .uuid = created_uuid };
    g_autoptr (PirwaReply) info = request (backend, &detail, &error);
    if (!info || info->details->has_cdrom || info->details->cpus != 2
        || info->details->memory_mb != 768 || !info->details->managed)
      goto cleanup;
  }
  if (!command (backend, PIRWA_START, created_uuid, &error)
      || !command (backend, PIRWA_FORCE_STOP, created_uuid, &error))
    goto cleanup;
  {
    PirwaRequest remove = { .operation = PIRWA_REMOVE, .uuid = created_uuid, .delete_disk = TRUE };
    g_autoptr (PirwaReply) removed = request (backend, &remove, &error);
    if (!removed || virStoragePoolNumOfVolumes (pool) != 0)
      goto cleanup;
  }
  g_clear_pointer (&created_uuid, g_free);
  g_print ("PASS: pause/resume, rename/resources, ISO eject, restart, and owned removal\n");
  {
    g_autoptr (GSubprocess) process
        = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error, "qemu-img", "create", "-f",
                            "qcow2", image, "4G", NULL);
    if (!process || !g_subprocess_wait_check (process, NULL, &error))
      goto cleanup;
  }
  gsize source_size = 0;
  if (!g_file_get_contents (image, &source_data, &source_size, &error))
    goto cleanup;
  op.options.source = image;
  op.options.import_image = TRUE;
  {
    g_autoptr (PirwaReply) imported = request (backend, &op, &error);
    if (!imported)
      goto cleanup;
    imported_uuid = g_strdup (imported->uuid);
  }
  if (!command (backend, PIRWA_FORCE_STOP, imported_uuid, &error))
    goto cleanup;
  {
    g_autofree char *after = NULL;
    gsize size = 0;
    if (!g_file_get_contents (image, &after, &size, &error) || size != source_size
        || memcmp (source_data, after, size))
      goto cleanup;
    PirwaRequest remove = { .operation = PIRWA_REMOVE, .uuid = imported_uuid, .delete_disk = TRUE };
    g_autoptr (PirwaReply) removed = request (backend, &remove, &error);
    if (!removed)
      goto cleanup;
  }
  g_clear_pointer (&imported_uuid, g_free);
  g_print ("PASS: standalone qcow2 stream import and original-image preservation\n");
  {
    g_autoptr (GSubprocess) process
        = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error, "qemu-img", "create", "-f",
                            "qcow2", "-F", "qcow2", "-b", image, backing, NULL);
    if (!process || !g_subprocess_wait_check (process, NULL, &error))
      goto cleanup;
    op.options.source = backing;
    g_autoptr (PirwaReply) rejected = request (backend, &op, &error);
    if (rejected || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
      goto cleanup;
    g_clear_error (&error);
  }
  g_print ("PASS: backing-chain rejection before allocation\n");
  ok = TRUE;
cleanup:
  if (!ok)
    g_printerr ("KVM test failed: %s\n",
                error ? error->message : "libvirt or verification failure");
  if (backend)
    pirwa_backend_stop (backend);
  if (connection)
    {
      cleanup_domain (connection, created_uuid);
      cleanup_domain (connection, duplicate_uuid);
      cleanup_domain (connection, imported_uuid);
      cleanup_domain (connection, shared_uuid);
      if (pool)
        {
          virStoragePoolRefresh (pool, 0);
          virStorageVolPtr *volumes = NULL;
          int count = virStoragePoolListAllVolumes (pool, &volumes, 0);
          for (int i = 0; i < count; i++)
            {
              virStorageVolDelete (volumes[i], 0);
              virStorageVolFree (volumes[i]);
            }
          free (volumes);
          virStoragePoolDestroy (pool);
          virStoragePoolUndefine (pool);
          virStoragePoolFree (pool);
        }
      virConnectClose (connection);
    }
  g_unlink (alias);
  g_unlink (image);
  g_unlink (backing);
  g_rmdir (directory);
  return ok ? 0 : 1;
}
