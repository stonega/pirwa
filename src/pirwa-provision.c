/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-provision.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <osinfo/osinfo.h>
#include <sys/stat.h>
#include <unistd.h>

static char *
ledger_path (virConnectPtr connection, const char *uuid)
{
  g_autofree char *uri = virConnectGetURI (connection);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, uri, -1);
  g_autofree char *file = g_strconcat (uuid, ".ini", NULL);
  return g_build_filename (g_get_user_data_dir (), "pirwa", "ownership", hash, file, NULL);
}

static gboolean
write_ledger (virConnectPtr connection, const char *uuid, const char *pool, const char *key,
              const char *path, GError **error)
{
  g_autofree char *file = ledger_path (connection, uuid);
  g_autofree char *directory = g_path_get_dirname (file);
  if (g_mkdir_with_parents (directory, 0700) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot create ownership record: %s", g_strerror (errno));
      return FALSE;
    }
  g_autoptr (GKeyFile) record = g_key_file_new ();
  g_key_file_set_string (record, "volume", "pool", pool);
  g_key_file_set_string (record, "volume", "key", key);
  g_key_file_set_string (record, "volume", "path", path);
  g_autofree char *data = g_key_file_to_data (record, NULL, NULL);
  return g_file_set_contents_full (
      file, data, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

gboolean
pirwa_provision_owned (virConnectPtr connection, const char *uuid, PirwaDetails *details)
{
  if (!details->managed)
    return FALSE;
  g_autofree char *expected = g_strdup_printf ("pirwa-%s.qcow2", uuid);
  if (g_strcmp0 (expected, details->volume_name))
    return FALSE;
  g_autofree char *file = ledger_path (connection, uuid);
  g_autoptr (GKeyFile) record = g_key_file_new ();
  if (!g_key_file_load_from_file (record, file, G_KEY_FILE_NONE, NULL))
    return FALSE;
  g_autofree char *pool = g_key_file_get_string (record, "volume", "pool", NULL);
  g_autofree char *key = g_key_file_get_string (record, "volume", "key", NULL);
  g_autofree char *path = g_key_file_get_string (record, "volume", "path", NULL);
  return g_strcmp0 (pool, details->pool_uuid) == 0 && g_strcmp0 (key, details->volume_key) == 0
         && g_strcmp0 (path, details->disk_path) == 0;
}

static virStoragePoolPtr
get_pool (virConnectPtr connection, const char *name, GError **error)
{
  virStoragePoolPtr pool = virStoragePoolLookupByName (connection, name);
  if (!pool)
    {
      g_autofree char *uri = virConnectGetURI (connection);
      if (!g_str_equal (name, "pirwa") || !g_str_equal (uri, "qemu:///session"))
        {
          pirwa_libvirt_error (error, "Select an existing active directory storage pool");
          return NULL;
        }
      g_autofree char *path = g_build_filename (g_get_user_data_dir (), "pirwa", "images", NULL);
      if (g_mkdir_with_parents (path, 0700) < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot create image directory: %s", g_strerror (errno));
          return NULL;
        }
      g_autofree char *xml = pirwa_config_pool (path);
      pool = virStoragePoolDefineXML (connection, xml, 0);
      if (!pool)
        {
          pirwa_libvirt_error (error, "Cannot define the Pirwa storage pool");
          return NULL;
        }
      if (virStoragePoolCreate (pool, 0) < 0)
        {
          pirwa_libvirt_error (error, "Cannot start the Pirwa storage pool");
          virStoragePoolUndefine (pool);
          virStoragePoolFree (pool);
          return NULL;
        }
    }
  g_autofree char *xml = virStoragePoolGetXMLDesc (pool, 0);
  xmlDoc *doc = xml ? xmlReadMemory (xml, strlen (xml), NULL, NULL, XML_PARSE_NONET) : NULL;
  xmlChar *type = doc ? xmlGetProp (xmlDocGetRootElement (doc), BAD_CAST "type") : NULL;
  gboolean directory = type && xmlStrEqual (type, BAD_CAST "dir");
  xmlFree (type);
  if (doc)
    xmlFreeDoc (doc);
  if (!directory || virStoragePoolIsActive (pool) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Choose an active directory storage pool");
      virStoragePoolFree (pool);
      return NULL;
    }
  return pool;
}

static gboolean
upload (virConnectPtr connection, virStorageVolPtr volume, int fd, guint64 size,
        GCancellable *cancellable, PirwaProgress progress, gpointer data, GError **error)
{
  virStreamPtr stream = virStreamNew (connection, 0);
  if (!stream)
    {
      pirwa_libvirt_error (error, "Cannot open upload stream");
      return FALSE;
    }
  gboolean ok = FALSE;
  g_autofree char *buffer = NULL;
  if (virStorageVolUpload (volume, stream, 0, size, 0) < 0)
    {
      pirwa_libvirt_error (error, "Cannot upload image");
      goto out;
    }
  buffer = g_malloc (1024 * 1024);
  guint64 total = 0;
  while (total < size)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;
      ssize_t count = read (fd, buffer, MIN ((guint64)(1024 * 1024), size - total));
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Source image changed or could not be read");
          goto out;
        }
      ssize_t sent = 0;
      while (sent < count)
        {
          int written = virStreamSend (stream, buffer + sent, count - sent);
          if (written <= 0)
            {
              pirwa_libvirt_error (error, "Image upload failed");
              goto out;
            }
          sent += written;
        }
      total += count;
      if (progress)
        progress (0.15 + 0.7 * total / size, "Copying disk image", data);
    }
  if (virStreamFinish (stream) < 0)
    {
      pirwa_libvirt_error (error, "Cannot finish image upload");
      goto out;
    }
  ok = TRUE;
out:
  if (!ok)
    virStreamAbort (stream);
  virStreamFree (stream);
  return ok;
}

char *
pirwa_provision_create (virConnectPtr connection, const PirwaCreate *o, GCancellable *cancellable,
                        PirwaProgress progress, gpointer data, GError **error)
{
  if (!pirwa_config_validate (o, error))
    return NULL;
  if (!o->source || !g_path_is_absolute (o->source))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Select a local ISO or qcow2 file");
      return NULL;
    }
  int fd = open (o->source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat source_stat;
  if (fd < 0 || fstat (fd, &source_stat) < 0 || !S_ISREG (source_stat.st_mode))
    {
      if (fd >= 0)
        close (fd);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Source must be a readable regular file, not a symbolic link");
      return NULL;
    }
  virStoragePoolPtr pool = NULL;
  virStorageVolPtr volume = NULL;
  virDomainPtr domain = NULL;
  char *result = NULL;
  g_autofree char *uuid = g_uuid_string_random ();
  g_autofree char *caps = NULL;
  g_autofree char *os_id = NULL;
  g_autofree char *volume_xml = NULL;
  g_autofree char *path = NULL;
  g_autofree char *xml = NULL;
  g_autofree char *passt_program = NULL;
  guint64 capacity = (guint64)o->disk_gb << 30;
  char pool_uuid[VIR_UUID_STRING_BUFLEN];
  if (progress)
    progress (0.02, "Checking host and installation media", data);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  caps = virConnectGetDomainCapabilities (connection, NULL, "x86_64", "q35", "kvm", 0);
  if (!caps)
    {
      pirwa_libvirt_error (error, "KVM with x86_64/q35 is required");
      goto out;
    }
  if (!pirwa_config_supports (
          caps, "/domainCapabilities/cpu/mode[@name='host-passthrough'][@supported='yes']")
      || !pirwa_config_supports (
          caps, "/domainCapabilities/devices/graphics/enum[@name='type']/value[.='spice']"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This host must support KVM host-passthrough CPU and SPICE graphics");
      goto out;
    }
  if (o->import_image)
    {
      if (!pirwa_qcow2_inspect (fd, &capacity, error))
        goto out;
    }
  else
    {
      OsinfoMedia *media = osinfo_media_create_from_location_with_flags (
          o->source, cancellable, OSINFO_MEDIA_DETECT_REQUIRE_BOOTABLE, error);
      if (!media)
        goto out;
      OsinfoLoader *loader = osinfo_loader_new ();
      osinfo_loader_process_default_path (loader, NULL);
      if (osinfo_db_identify_media (osinfo_loader_get_db (loader), media))
        {
          OsinfoOs *os = osinfo_media_get_os (media);
          if (os)
            os_id = g_strdup (osinfo_entity_get_id (OSINFO_ENTITY (os)));
        }
      const char *arch = osinfo_media_get_architecture (media);
      if (arch && !g_str_equal (arch, "x86_64") && !g_str_equal (arch, "i686")
          && !g_str_equal (arch, "all"))
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Select x86_64 Linux installation media");
      g_object_unref (loader);
      g_object_unref (media);
      if (*error)
        goto out;
    }
  pool = get_pool (connection, o->pool && *o->pool ? o->pool : "pirwa", error);
  if (!pool)
    goto out;
  if (virStoragePoolGetUUIDString (pool, pool_uuid) < 0)
    {
      pirwa_libvirt_error (error, "Cannot identify storage pool");
      goto out;
    }
  volume_xml = pirwa_config_volume (uuid, capacity);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  if (progress)
    progress (0.1, "Allocating disk", data);
  volume = virStorageVolCreateXML (pool, volume_xml, 0);
  if (!volume)
    {
      pirwa_libvirt_error (error, "Cannot allocate disk");
      goto out;
    }
  if (o->import_image
      && !upload (connection, volume, fd, source_stat.st_size, cancellable, progress, data, error))
    goto out;
  struct stat after;
  if (fstat (fd, &after) < 0 || after.st_size != source_stat.st_size
      || after.st_mtim.tv_sec != source_stat.st_mtim.tv_sec
      || after.st_mtim.tv_nsec != source_stat.st_mtim.tv_nsec)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Source changed during creation; try again with a stable copy");
      goto out;
    }
  path = virStorageVolGetPath (volume);
  const char *key = virStorageVolGetKey (volume);
  if (!path || !key)
    {
      pirwa_libvirt_error (error, "Cannot identify allocated disk");
      goto out;
    }
  passt_program = g_find_program_in_path ("passt");
  gboolean passt
      = passt_program
        && pirwa_config_supports (
            caps,
            "/domainCapabilities/devices/interface/enum[@name='backendType']/value[.='passt']");
  xml = pirwa_config_domain (
      o, uuid, path, pool_uuid, key,
      !o->legacy_bios
          && pirwa_config_supports (caps,
                                    "/domainCapabilities/os/enum[@name='firmware']/value[.='efi']"),
      passt, os_id);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  if (progress)
    progress (0.9, "Defining virtual machine", data);
  domain = virDomainDefineXML (connection, xml);
  if (!domain)
    {
      pirwa_libvirt_error (error, "Cannot define virtual machine");
      goto out;
    }
  if (!write_ledger (connection, uuid, pool_uuid, key, path, error))
    goto out;
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  if (virDomainCreate (domain) < 0)
    {
      pirwa_libvirt_error (error, "Cannot start virtual machine");
      goto out;
    }
  result = g_strdup (uuid);
  if (progress)
    progress (1.0, "Virtual machine is running", data);
out:
  if (!result)
    {
      gboolean safe_to_delete = TRUE;
      g_autofree char *defined
          = domain ? virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE) : NULL;
      if (domain
          && (!defined
              || virDomainUndefineFlags (domain,
                                         strstr (defined, "<nvram") ? VIR_DOMAIN_UNDEFINE_NVRAM : 0)
                     < 0))
        {
          /* Never remove storage if the definition could not be rolled back. */
          safe_to_delete = FALSE;
          g_prefix_error (error,
                          "Rollback could not remove the VM definition; its disk was kept. ");
        }
      if (volume && safe_to_delete && virStorageVolDelete (volume, 0) < 0)
        {
          safe_to_delete = FALSE;
          g_prefix_error (
              error,
              "Rollback could not remove the allocated disk; its ownership record was kept. ");
        }
      if (safe_to_delete)
        {
          g_autofree char *file = ledger_path (connection, uuid);
          g_unlink (file);
        }
    }
  if (domain)
    virDomainFree (domain);
  if (volume)
    virStorageVolFree (volume);
  if (pool)
    virStoragePoolFree (pool);
  close (fd);
  return result;
}

static gboolean
references_volume (const char *xml, const char *path, const char *name)
{
  if (!xml)
    return TRUE;
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc)
    return TRUE;
  xmlXPathContext *context = xmlXPathNewContext (doc);
  xmlXPathObject *sources = xmlXPathEvalExpression (BAD_CAST "//disk//source", context);
  struct stat target;
  gboolean used = g_stat (path, &target) < 0;
  if (!sources || !sources->nodesetval)
    used = TRUE;
  for (int i = 0; !used && i < sources->nodesetval->nodeNr; i++)
    {
      xmlNode *source = sources->nodesetval->nodeTab[i];
      xmlChar *file = xmlGetProp (source, BAD_CAST "file");
      xmlChar *volume = xmlGetProp (source, BAD_CAST "volume");
      struct stat other;
      if (volume && g_str_equal ((const char *)volume, name))
        used = TRUE;
      if (file
          && (g_str_equal ((const char *)file, path) || g_stat ((const char *)file, &other) < 0
              || (target.st_dev == other.st_dev && target.st_ino == other.st_ino)))
        used = TRUE;
      xmlFree (file);
      xmlFree (volume);
    }
  if (sources)
    xmlXPathFreeObject (sources);
  xmlXPathFreeContext (context);
  xmlFreeDoc (doc);
  return used;
}

gboolean
pirwa_provision_remove (virConnectPtr connection, virDomainPtr domain, const char *uuid,
                        gboolean delete_disk, GError **error)
{
  if (virDomainIsActive (domain) != 0 || virDomainIsPersistent (domain) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Power off the persistent VM before removing it");
      return FALSE;
    }
  g_autofree char *xml = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  if (!xml)
    {
      pirwa_libvirt_error (error, "Cannot read definition");
      return FALSE;
    }
  g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, error);
  if (!details)
    return FALSE;
  virStoragePoolPtr pool = NULL;
  virStorageVolPtr volume = NULL;
  gboolean ok = FALSE;
  if (delete_disk)
    {
      if (!pirwa_provision_owned (connection, uuid, details))
        {
          g_set_error_literal (
              error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
              "Disk ownership could not be verified; remove the definition and keep its storage");
          goto out;
        }
      pool = virStoragePoolLookupByUUIDString (connection, details->pool_uuid);
      volume = pool ? virStorageVolLookupByName (pool, details->volume_name) : NULL;
      g_autofree char *path = volume ? virStorageVolGetPath (volume) : NULL;
      if (!volume || g_strcmp0 (path, details->disk_path)
          || g_strcmp0 (virStorageVolGetKey (volume), details->volume_key))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "The owned storage volume has changed; it was kept");
          goto out;
        }
      virDomainPtr *domains = NULL;
      int count = virConnectListAllDomains (connection, &domains, 0);
      if (count < 0)
        {
          pirwa_libvirt_error (error, "Cannot check shared disks");
          goto out;
        }
      gboolean shared = FALSE;
      for (int i = 0; i < count; i++)
        {
          char other_uuid[VIR_UUID_STRING_BUFLEN];
          if (virDomainGetUUIDString (domains[i], other_uuid) < 0)
            shared = TRUE;
          else if (!g_str_equal (other_uuid, uuid))
            {
              g_autofree char *other = virDomainGetXMLDesc (domains[i], 0);
              if (references_volume (other, path, details->volume_name))
                shared = TRUE;
              if (virDomainIsPersistent (domains[i]) == 1)
                {
                  g_autofree char *inactive
                      = virDomainGetXMLDesc (domains[i], VIR_DOMAIN_XML_INACTIVE);
                  if (references_volume (inactive, path, details->volume_name))
                    shared = TRUE;
                }
            }
          virDomainFree (domains[i]);
        }
      free (domains);
      if (shared)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                               "Another VM may use this disk; storage was kept");
          goto out;
        }
    }
  if (virDomainUndefineFlags (domain, details->has_nvram ? VIR_DOMAIN_UNDEFINE_NVRAM : 0) < 0)
    {
      pirwa_libvirt_error (error, "Cannot remove virtual machine");
      goto out;
    }
  if (volume && virStorageVolDelete (volume, 0) < 0)
    {
      pirwa_libvirt_error (error, "Definition removed; disk deletion failed and storage was kept");
      goto out;
    }
  if (delete_disk)
    {
      g_autofree char *file = ledger_path (connection, uuid);
      g_unlink (file);
    }
  ok = TRUE;
out:
  if (volume)
    virStorageVolFree (volume);
  if (pool)
    virStoragePoolFree (pool);
  return ok;
}
