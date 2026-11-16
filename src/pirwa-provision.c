/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-provision.h"
#include "pirwa-import.h"
#include "pirwa-network.h"
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
          if (g_cancellable_set_error_if_cancelled (cancellable, error))
            goto out;
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
  int driver_fd = -1;
  struct stat driver_stat;
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
          caps, "/domainCapabilities/devices/graphics/enum[@name='type']/value[.='spice']")
      || !pirwa_config_supports (
          caps, "/domainCapabilities/devices/video/enum[@name='modelType']/value[.='qxl']"))
    {
      g_set_error_literal (
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "This host must support KVM host-passthrough CPU, SPICE and QXL graphics");
      goto out;
    }
  if (!pirwa_config_check_host (o, caps, error))
    goto out;
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
      if (arch && !g_str_equal (arch, "x86_64") && !g_str_equal (arch, "all")
          && (o->guest != PIRWA_GUEST_LINUX || !g_str_equal (arch, "i686")))
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Select x86_64 installation media for this guest profile");
      g_object_unref (loader);
      g_object_unref (media);
      if (*error)
        goto out;
    }
  if (o->guest == PIRWA_GUEST_LINUX && os_id
      && g_str_has_prefix (os_id, "http://microsoft.com/win/"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Windows media detected. Choose the matching Windows profile and "
                           "a virtio-win driver ISO in Configuration.");
      goto out;
    }
  if (o->guest != PIRWA_GUEST_LINUX)
    {
      const char *expected = o->guest == PIRWA_GUEST_WINDOWS_11 ? "http://microsoft.com/win/11"
                                                                : "http://microsoft.com/win/10";
      if (os_id && g_strcmp0 (os_id, expected) != 0)
        {
          g_set_error_literal (
              error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
              "The detected installation media does not match the Windows profile");
          goto out;
        }
      if (!os_id)
        os_id = g_strdup (expected);
      driver_fd = open (o->driver_iso, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (driver_fd < 0 || fstat (driver_fd, &driver_stat) < 0 || !S_ISREG (driver_stat.st_mode))
        {
          g_set_error_literal (
              error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
              "The driver ISO must be a readable regular file, not a symbolic link");
          goto out;
        }
      g_autoptr (OsinfoMedia) drivers
          = osinfo_media_create_from_location_with_flags (o->driver_iso, cancellable, 0, error);
      if (!drivers)
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
  if (driver_fd >= 0
      && (fstat (driver_fd, &after) < 0 || after.st_size != driver_stat.st_size
          || after.st_mtim.tv_sec != driver_stat.st_mtim.tv_sec
          || after.st_mtim.tv_nsec != driver_stat.st_mtim.tv_nsec))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Driver ISO changed during creation; try again with a stable copy");
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
  if (!pirwa_network_start (connection, domain, cancellable, error))
    goto out;
  result = g_strdup (uuid);
  if (progress)
    progress (1.0, "Virtual machine is running", data);
out:
  if (!result)
    {
      g_autoptr (GError) rollback_error = NULL;
      if (pirwa_provision_rollback (connection, domain, volume, &rollback_error))
        {
          g_autofree char *file = ledger_path (connection, uuid);
          g_unlink (file);
        }
      else
        g_prefix_error (error, "Rollback incomplete; ownership record and storage were kept (%s). ",
                        rollback_error->message);
    }
  if (domain)
    virDomainFree (domain);
  if (volume)
    virStorageVolFree (volume);
  if (pool)
    virStoragePoolFree (pool);
  if (driver_fd >= 0)
    close (driver_fd);
  close (fd);
  return result;
}

/* Flatpak's /app and /usr belong to its deployments, not the host. Copy any
 * referenced firmware so the imported guest survives uninstalling Boxes. */
static char *
flatpak_info (const char *option, const char *ref, GCancellable *cancellable, GError **error)
{
  g_autoptr (GSubprocess) process
      = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error,
                          "flatpak", "info", option, ref, NULL);
  char *output = NULL;
  if (!process)
    return NULL;
  if (!g_subprocess_communicate_utf8 (process, NULL, cancellable, &output, NULL, error))
    {
      g_subprocess_force_exit (process);
      g_free (output);
      return NULL;
    }
  if (!g_subprocess_get_successful (process) || !output || !*g_strstrip (output))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Cannot locate the installed Boxes Flatpak or its runtime");
      g_free (output);
      return NULL;
    }
  return output;
}

static char *
boxes_file (const char *path, GCancellable *cancellable, GError **error)
{
  if (g_str_has_prefix (path, "/app/") || g_str_has_prefix (path, "/usr/"))
    {
      g_autofree char *runtime = NULL;
      if (g_str_has_prefix (path, "/usr/"))
        {
          runtime = flatpak_info ("--show-runtime", "org.gnome.Boxes", cancellable, error);
          if (!runtime)
            return NULL;
        }
      g_autofree char *location = flatpak_info (
          "--show-location", runtime ? runtime : "org.gnome.Boxes", cancellable, error);
      if (!location)
        return NULL;
      g_autofree char *file = g_build_filename (location, "files", path + 5, NULL);
      char *canonical = realpath (file, NULL);
      if (!canonical)
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot locate Boxes firmware: %s", g_strerror (errno));
      return canonical;
    }
  const char *suffix = NULL;
  const char *directory = NULL;
  if (g_str_has_prefix (path, "/var/data/"))
    {
      directory = "data";
      suffix = path + strlen ("/var/data/");
    }
  else if (g_str_has_prefix (path, "/var/config/"))
    {
      directory = "config";
      suffix = path + strlen ("/var/config/");
    }
  return directory ? g_build_filename (g_get_home_dir (), ".var", "app", "org.gnome.Boxes",
                                       directory, suffix, NULL)
                   : g_strdup (path);
}

static gboolean
boxes_stopped (virDomainPtr domain, GError **error)
{
  int state, reason;
  if (virDomainGetState (domain, &state, &reason, 0) < 0)
    {
      pirwa_libvirt_error (error, "Cannot check the source machine's state");
      return FALSE;
    }
  if (state != VIR_DOMAIN_SHUTOFF || virDomainIsPersistent (domain) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Shut down this machine in Boxes before importing. Leave Boxes open "
                           "and do not start the machine until import finishes.");
      return FALSE;
    }
  return TRUE;
}

static gboolean
same_file (const char *path, const struct stat *before)
{
  struct stat after;
  return lstat (path, &after) == 0 && S_ISREG (after.st_mode) && before->st_dev == after.st_dev
         && before->st_ino == after.st_ino && before->st_size == after.st_size
         && before->st_mtim.tv_sec == after.st_mtim.tv_sec
         && before->st_mtim.tv_nsec == after.st_mtim.tv_nsec
         && before->st_ctim.tv_sec == after.st_ctim.tv_sec
         && before->st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

char *
pirwa_provision_import_boxes (virConnectPtr connection, const char *source_uri,
                              const char *source_uuid, GCancellable *cancellable,
                              PirwaProgress progress, gpointer data, GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;
  g_autofree char *destination_uri = virConnectGetURI (connection);
  if (!source_uri || !source_uuid || !g_uuid_string_is_valid (source_uuid)
      || (!g_str_has_prefix (source_uri, "qemu+unix:///session?socket=")
          && !g_str_has_prefix (source_uri, "test:///"))
      || (!g_str_equal (destination_uri, "qemu:///session")
          && !g_str_has_prefix (destination_uri, "test:///")))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Boxes import requires a local source and the Session destination");
      return NULL;
    }
  virConnectPtr source = virConnectOpenReadOnly (source_uri);
  if (!source)
    {
      pirwa_libvirt_error (error,
                           "Open Boxes Flatpak, shut down the guest, then refresh the library");
      return NULL;
    }
  virDomainPtr original = virDomainLookupByUUIDString (source, source_uuid);
  virStoragePoolPtr pool = NULL;
  virStorageVolPtr volume = NULL;
  virDomainPtr domain = NULL;
  g_autoptr (PirwaImport) plan = NULL;
  g_autofree char *source_xml = NULL;
  g_autofree char *disk = NULL;
  g_autofree char *loader = NULL;
  g_autofree char *nvram = NULL;
  g_autofree char *firmware_directory = NULL;
  g_autofree char *loader_target = NULL;
  g_autofree char *nvram_target = NULL;
  g_autofree char *volume_xml = NULL;
  g_autofree char *path = NULL;
  g_autofree char *xml = NULL;
  g_autofree char *after_xml = NULL;
  int fd = -1;
  char *result = NULL;
  struct stat before, loader_before, nvram_before;
  guint64 capacity;
  char pool_uuid[VIR_UUID_STRING_BUFLEN];
  if (!original)
    {
      pirwa_libvirt_error (error, "The source machine no longer exists; refresh the library");
      goto out;
    }
  if (!boxes_stopped (original, error))
    goto out;
  int snapshots = virDomainSnapshotNum (original, 0);
  if (snapshots < 0)
    {
      pirwa_libvirt_error (error, "Cannot inspect source snapshots");
      goto out;
    }
  int saved = virDomainHasManagedSaveImage (original, 0);
  if (saved < 0)
    {
      pirwa_libvirt_error (error, "Cannot inspect source snapshots and saved state");
      goto out;
    }
  if (snapshots || saved)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Machines with snapshots or saved memory state cannot be imported. "
                           "Shut down the guest fully and use a standalone disk copy.");
      goto out;
    }
  source_xml = virDomainGetXMLDesc (original, VIR_DOMAIN_XML_INACTIVE);
  if (!source_xml)
    {
      pirwa_libvirt_error (error, "Cannot read the source definition");
      goto out;
    }
  plan = pirwa_import_prepare (source_xml, error);
  if (!plan)
    goto out;
  disk = boxes_file (plan->disk, cancellable, error);
  if (!disk)
    goto out;
  fd = open (disk, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 || fstat (fd, &before) < 0 || !S_ISREG (before.st_mode))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "The source disk must be a readable regular file, not a symbolic link");
      goto out;
    }
  struct flock lock = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
  if (fcntl (fd, F_SETLK, &lock) < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "The source disk is in use; shut down its machine before importing");
      goto out;
    }
  if (!pirwa_qcow2_inspect (fd, &capacity, error))
    goto out;
  guint32 internal_snapshots;
  if (pread (fd, &internal_snapshots, sizeof internal_snapshots, 60) != sizeof internal_snapshots
      || internal_snapshots)
    {
      g_set_error_literal (
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "The disk contains internal snapshots. Import a standalone copy instead.");
      goto out;
    }
  if (progress)
    progress (0.02, "Checking the stopped Boxes machine", data);
  if (plan->loader)
    {
      loader = boxes_file (plan->loader, cancellable, error);
      if (!loader)
        goto out;
      nvram = boxes_file (plan->nvram, cancellable, error);
      if (!nvram)
        goto out;
      if (lstat (loader, &loader_before) < 0 || lstat (nvram, &nvram_before) < 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Cannot read the source firmware or NVRAM file");
          goto out;
        }
      g_autofree char *parent
          = g_build_filename (g_get_user_data_dir (), "pirwa", "firmware", NULL);
      if (g_mkdir_with_parents (parent, 0700) < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot create firmware directory: %s", g_strerror (errno));
          goto out;
        }
      firmware_directory = g_build_filename (parent, "import-XXXXXX", NULL);
      if (!g_mkdtemp (firmware_directory))
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot allocate firmware directory: %s", g_strerror (errno));
          g_clear_pointer (&firmware_directory, g_free);
          goto out;
        }
      loader_target = g_build_filename (firmware_directory, "loader.fd", NULL);
      nvram_target = g_build_filename (firmware_directory, "nvram.fd", NULL);
      if (!pirwa_import_copy_firmware (loader, loader_target, cancellable, error)
          || !pirwa_import_copy_firmware (nvram, nvram_target, cancellable, error))
        goto out;
    }
  pool = get_pool (connection, "pirwa", error);
  if (!pool)
    goto out;
  if (virStoragePoolGetUUIDString (pool, pool_uuid) < 0)
    {
      pirwa_libvirt_error (error, "Cannot identify the destination pool");
      goto out;
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  volume_xml = pirwa_config_volume (plan->uuid, capacity);
  volume = virStorageVolCreateXML (pool, volume_xml, 0);
  if (!volume)
    {
      pirwa_libvirt_error (error, "Cannot allocate the imported disk");
      goto out;
    }
  if (!upload (connection, volume, fd, before.st_size, cancellable, progress, data, error))
    goto out;
  after_xml = virDomainGetXMLDesc (original, VIR_DOMAIN_XML_INACTIVE);
  if (!after_xml)
    {
      pirwa_libvirt_error (error, "Cannot recheck the source definition");
      goto out;
    }
  if (!boxes_stopped (original, error))
    goto out;
  if (g_strcmp0 (source_xml, after_xml) || !same_file (disk, &before)
      || (loader && (!same_file (loader, &loader_before) || !same_file (nvram, &nvram_before))))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "The source changed during import; its original files were kept");
      goto out;
    }
  path = virStorageVolGetPath (volume);
  const char *key = virStorageVolGetKey (volume);
  if (!path || !key)
    {
      pirwa_libvirt_error (error, "Cannot identify the imported disk");
      goto out;
    }
  xml = pirwa_import_domain (plan, path, loader_target, nvram_target, pool_uuid, key);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  if (progress)
    progress (0.9, "Registering the imported machine in Session", data);
  domain = virDomainDefineXML (connection, xml);
  if (!domain)
    {
      pirwa_libvirt_error (error, "The host cannot use this machine's configuration");
      goto out;
    }
  if (!write_ledger (connection, plan->uuid, pool_uuid, key, path, error)
      || g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  /* Leave the copy stopped for review. Booting a copied guest can have network
   * effects; starting it is the user's next explicit operation. */
  result = g_strdup (plan->uuid);
  if (progress)
    progress (1.0, "Import complete. Select Start to check the copied machine.", data);
out:
  if (!result)
    {
      g_autoptr (GError) rollback_error = NULL;
      if (pirwa_provision_rollback (connection, domain, volume, &rollback_error))
        {
          if (plan)
            {
              g_autofree char *file = ledger_path (connection, plan->uuid);
              g_unlink (file);
            }
          if (firmware_directory)
            {
              if (loader_target)
                g_unlink (loader_target);
              if (nvram_target)
                g_unlink (nvram_target);
              g_rmdir (firmware_directory);
            }
        }
      else
        g_prefix_error (error, "Import cleanup incomplete; copied resources were kept (%s). ",
                        rollback_error->message);
    }
  if (fd >= 0)
    close (fd);
  if (domain)
    virDomainFree (domain);
  if (volume)
    virStorageVolFree (volume);
  if (pool)
    virStoragePoolFree (pool);
  if (original)
    virDomainFree (original);
  virConnectClose (source);
  return result;
}

gboolean
pirwa_provision_references (const char *xml, const char *path, const char *name)
{
  if (!xml)
    return TRUE;
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc)
    return TRUE;
  if (doc->intSubset || doc->extSubset)
    {
      xmlFreeDoc (doc);
      return TRUE;
    }
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
      if (file)
        {
          if (g_str_equal ((const char *)file, path))
            used = TRUE;
          else if (g_stat ((const char *)file, &other) == 0)
            used |= target.st_dev == other.st_dev && target.st_ino == other.st_ino;
          else if (errno != ENOENT && errno != ENOTDIR)
            used = TRUE; /* Inaccessible paths remain ambiguous; absent unrelated files do not. */
        }
      xmlFree (file);
      xmlFree (volume);
    }
  if (sources)
    xmlXPathFreeObject (sources);
  xmlXPathFreeContext (context);
  xmlFreeDoc (doc);
  return used;
}

static gboolean
volume_unused (virConnectPtr connection, virStorageVolPtr volume, const char *skip_uuid,
               GCancellable *cancellable, GError **error)
{
  g_autofree char *path = virStorageVolGetPath (volume);
  const char *name = virStorageVolGetName (volume);
  if (!path || !name)
    {
      pirwa_libvirt_error (error, "Cannot identify storage");
      return FALSE;
    }
  virDomainPtr *domains = NULL;
  int count = virConnectListAllDomains (connection, &domains, 0);
  if (count < 0)
    {
      pirwa_libvirt_error (error, "Cannot check shared disks");
      return FALSE;
    }
  gboolean shared = FALSE;
  gboolean cancelled = FALSE;
  for (int i = 0; i < count; i++)
    {
      if (!shared && !cancelled)
        cancelled = g_cancellable_set_error_if_cancelled (cancellable, error);
      if (shared || cancelled)
        {
          virDomainFree (domains[i]);
          continue;
        }
      char other_uuid[VIR_UUID_STRING_BUFLEN];
      if (virDomainGetUUIDString (domains[i], other_uuid) < 0)
        shared = TRUE;
      else if (!skip_uuid || !g_str_equal (other_uuid, skip_uuid))
        {
          g_autofree char *current = virDomainGetXMLDesc (domains[i], 0);
          if (pirwa_provision_references (current, path, name))
            shared = TRUE;
          int persistent = virDomainIsPersistent (domains[i]);
          if (persistent < 0)
            shared = TRUE;
          if (persistent == 1)
            {
              g_autofree char *inactive = virDomainGetXMLDesc (domains[i], VIR_DOMAIN_XML_INACTIVE);
              if (pirwa_provision_references (inactive, path, name))
                shared = TRUE;
            }
        }
      virDomainFree (domains[i]);
    }
  free (domains);
  if (cancelled)
    return FALSE;
  if (shared)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                         "Another VM may use this disk; storage was kept");
  return !shared;
}

gboolean
pirwa_provision_rollback (virConnectPtr connection, virDomainPtr domain, virStorageVolPtr volume,
                          GError **error)
{
  char uuid[VIR_UUID_STRING_BUFLEN];
  g_autofree char *xml = NULL;
  g_autoptr (PirwaDetails) details = NULL;
  if (domain)
    {
      if (virDomainIsActive (domain) != 0)
        {
          g_set_error_literal (
              error, G_IO_ERROR, G_IO_ERROR_BUSY,
              "VM is running or its state is unknown; its definition and disk were kept");
          return FALSE;
        }
      if (virDomainGetUUIDString (domain, uuid) < 0)
        {
          pirwa_libvirt_error (error, "Cannot identify rollback domain");
          return FALSE;
        }
      xml = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
      if (!xml)
        {
          pirwa_libvirt_error (error, "Cannot inspect rollback domain");
          return FALSE;
        }
      details = pirwa_config_parse (xml, error);
      if (!details)
        return FALSE;
    }
  if (volume && !volume_unused (connection, volume, domain ? uuid : NULL, NULL, error))
    return FALSE;
  unsigned int flags = details && details->has_nvram ? VIR_DOMAIN_UNDEFINE_NVRAM : 0;
  if (details && details->has_tpm)
    flags |= VIR_DOMAIN_UNDEFINE_TPM;
  if (domain && virDomainUndefineFlags (domain, flags) < 0)
    {
      pirwa_libvirt_error (error, "Cannot remove rollback definition");
      return FALSE;
    }
  /* Scan again without excluding the old UUID: a racing start can leave a
   * running transient domain after undefine. Never unlink its backing volume. */
  if (volume && !volume_unused (connection, volume, NULL, NULL, error))
    return FALSE;
  if (volume && virStorageVolDelete (volume, 0) < 0)
    {
      pirwa_libvirt_error (error, "Cannot remove rollback storage");
      return FALSE;
    }
  return TRUE;
}

gboolean
pirwa_provision_remove (virConnectPtr connection, virDomainPtr domain, const char *uuid,
                        gboolean delete_disk, GCancellable *cancellable, GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;
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
      if (!volume_unused (connection, volume, uuid, cancellable, error))
        goto out;
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;
  unsigned int flags = details->has_nvram
                           ? (details->has_tpm && !delete_disk ? VIR_DOMAIN_UNDEFINE_KEEP_NVRAM
                                                               : VIR_DOMAIN_UNDEFINE_NVRAM)
                           : 0;
  if (details->has_tpm)
    flags |= delete_disk ? VIR_DOMAIN_UNDEFINE_TPM : VIR_DOMAIN_UNDEFINE_KEEP_TPM;
  if (virDomainUndefineFlags (domain, flags) < 0)
    {
      pirwa_libvirt_error (error, "Cannot remove virtual machine");
      goto out;
    }
  if (volume && !volume_unused (connection, volume, NULL, NULL, error))
    {
      g_prefix_error (error, "Definition removed; storage was kept. ");
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
