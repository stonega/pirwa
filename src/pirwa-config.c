/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-config.h"
#include <errno.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static xmlNode *
element (xmlNode *parent, const char *name, const char *content)
{
  return xmlNewTextChild (parent, NULL, BAD_CAST name, BAD_CAST content);
}

static void
attribute (xmlNode *node, const char *name, const char *value)
{
  xmlSetProp (node, BAD_CAST name, BAD_CAST value);
}

static char *
serialize (xmlDoc *doc)
{
  xmlChar *buffer = NULL;
  int size = 0;
  xmlDocDumpMemory (doc, &buffer, &size);
  char *result = g_strndup ((const char *)buffer, size);
  xmlFree (buffer);
  xmlFreeDoc (doc);
  return result;
}

static xmlDoc *
parse (const char *xml, GError **error)
{
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), "domain.xml", NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (doc == NULL || doc->intSubset || doc->extSubset)
    {
      if (doc)
        xmlFreeDoc (doc);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid domain XML");
      return NULL;
    }
  return doc;
}

static xmlXPathObject *
query (xmlDoc *doc, const char *expression)
{
  xmlXPathContext *context = xmlXPathNewContext (doc);
  xmlXPathRegisterNs (context, BAD_CAST "p", BAD_CAST PIRWA_METADATA_NS);
  xmlXPathRegisterNs (context, BAD_CAST "o", BAD_CAST PIRWA_OSINFO_NS);
  xmlXPathObject *result = xmlXPathEvalExpression (BAD_CAST expression, context);
  xmlXPathFreeContext (context);
  return result;
}

static char *
value (xmlDoc *doc, const char *expression)
{
  xmlXPathObject *result = query (doc, expression);
  xmlChar *text = result ? xmlXPathCastToString (result) : NULL;
  char *copy = text && *text ? g_strdup ((const char *)text) : NULL;
  xmlFree (text);
  if (result)
    xmlXPathFreeObject (result);
  return copy;
}

gboolean
pirwa_config_supports (const char *xml, const char *expression)
{
  xmlDoc *doc = parse (xml, NULL);
  if (!doc)
    return FALSE;
  xmlXPathObject *result = query (doc, expression);
  gboolean supported = result && xmlXPathCastToBoolean (result);
  if (result)
    xmlXPathFreeObject (result);
  xmlFreeDoc (doc);
  return supported;
}

char *
pirwa_config_os_id (const char *xml)
{
  xmlDoc *doc = parse (xml, NULL);
  if (!doc)
    return NULL;
  /* virDomainGetMetadata strips namespace prefixes from the returned fragment. */
  char *id = value (doc, "/domain/metadata/o:libosinfo/o:os/@id | /o:libosinfo/o:os/@id"
                         " | /libosinfo/os/@id");
  xmlFreeDoc (doc);
  return id;
}

int
pirwa_config_spice_index (const char *xml)
{
  xmlDoc *doc = parse (xml, NULL);
  if (!doc)
    return -1;
  xmlXPathObject *result = query (doc, "/domain/devices/graphics");
  int index = -1;
  if (result && result->nodesetval)
    for (int i = 0; i < result->nodesetval->nodeNr; i++)
      {
        xmlChar *type = xmlGetProp (result->nodesetval->nodeTab[i], BAD_CAST "type");
        gboolean spice = xmlStrEqual (type, BAD_CAST "spice");
        xmlFree (type);
        if (spice)
          {
            index = i;
            break;
          }
      }
  if (result)
    xmlXPathFreeObject (result);
  xmlFreeDoc (doc);
  return index;
}

gboolean
pirwa_config_validate (const PirwaCreate *options, GError **error)
{
  if (!options->name || !*options->name || !g_utf8_validate (options->name, -1, NULL)
      || g_utf8_strlen (options->name, -1) > 80 || strchr (options->name, '/')
      || strchr (options->name, '\\'))
    goto invalid;
  for (const char *p = options->name; *p; p = g_utf8_next_char (p))
    if (g_unichar_iscntrl (g_utf8_get_char (p)))
      goto invalid;
  if (options->cpus < 1 || options->cpus > 64 || options->memory_mb < 256
      || options->memory_mb > 262144 || options->disk_gb < 4 || options->disk_gb > 2048)
    goto invalid;
  if (options->guest < PIRWA_GUEST_LINUX || options->guest > PIRWA_GUEST_WINDOWS_11)
    goto invalid;
  if (options->guest != PIRWA_GUEST_LINUX)
    {
      if (options->import_image)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "Windows profiles require an installation ISO; importing Windows "
                               "disks with new hardware or TPM state is not supported");
          return FALSE;
        }
      if (!options->driver_iso || !g_path_is_absolute (options->driver_iso))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "Choose a local virtio-win driver ISO for Windows installation");
          return FALSE;
        }
      guint min_memory = options->guest == PIRWA_GUEST_WINDOWS_11 ? 4096 : 2048;
      guint min_disk = options->guest == PIRWA_GUEST_WINDOWS_11 ? 64 : 32;
      guint min_cpus = options->guest == PIRWA_GUEST_WINDOWS_11 ? 2 : 1;
      if (options->cpus < min_cpus || options->memory_mb < min_memory
          || options->disk_gb < min_disk)
        {
          g_set_error (
              error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
              "This Windows profile requires at least %u CPUs, %u MiB RAM and a %u GiB disk",
              min_cpus, min_memory, min_disk);
          return FALSE;
        }
      if (options->guest == PIRWA_GUEST_WINDOWS_11 && options->legacy_bios)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "Windows 11 requires UEFI, Secure Boot and TPM 2.0");
          return FALSE;
        }
    }
  return TRUE;
invalid:
  g_set_error_literal (
      error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
      "Use a name of 1–80 characters, 1–64 CPUs, 256–262144 MiB RAM, and a 4–2048 GiB disk.");
  return FALSE;
}

gboolean
pirwa_config_check_host (const PirwaCreate *options, const char *caps, GError **error)
{
  if (options->guest != PIRWA_GUEST_WINDOWS_11)
    return TRUE;
  if (!pirwa_config_supports (caps, "/domainCapabilities/os/enum[@name='firmware']/value[.='efi']")
      || !pirwa_config_supports (caps, "/domainCapabilities/devices/tpm[@supported='yes']"
                                       "[enum[@name='model']/value='tpm-crb']"
                                       "[enum[@name='backendModel']/value='emulator']"
                                       "[enum[@name='backendVersion']/value='2.0']"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Windows 11 requires UEFI firmware and an emulated TPM 2.0. "
                           "Install edk2-ovmf, swtpm and swtpm-tools on the libvirt host.");
      return FALSE;
    }
  /* Libvirt before 12.1 lacks firmwareFeatures. In that case definition-time
   * firmware selection checks the requested Secure Boot/key combination. */
  if (pirwa_config_supports (caps, "/domainCapabilities/os/firmwareFeatures")
      && !pirwa_config_supports (
          caps, "/domainCapabilities/os/firmwareFeatures[@supported='yes']"
                "[enum[@name='secureBoot']/value='yes'][enum[@name='enrolledKeys']/value='yes']"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Windows 11 requires Secure Boot firmware with enrolled keys. "
                           "Install the distribution's edk2-ovmf firmware package.");
      return FALSE;
    }
  return TRUE;
}

char *
pirwa_config_volume (const char *uuid, guint64 capacity)
{
  xmlDoc *doc = xmlNewDoc (BAD_CAST "1.0");
  xmlNode *root = xmlNewNode (NULL, BAD_CAST "volume");
  xmlDocSetRootElement (doc, root);
  g_autofree char *name = g_strdup_printf ("pirwa-%s.qcow2", uuid);
  g_autofree char *bytes = g_strdup_printf ("%" G_GUINT64_FORMAT, capacity);
  element (root, "name", name);
  attribute (element (root, "capacity", bytes), "unit", "bytes");
  element (root, "allocation", "0");
  attribute (element (element (root, "target", NULL), "format", NULL), "type", "qcow2");
  return serialize (doc);
}

char *
pirwa_config_pool (const char *path)
{
  xmlDoc *doc = xmlNewDoc (BAD_CAST "1.0");
  xmlNode *root = xmlNewNode (NULL, BAD_CAST "pool");
  xmlDocSetRootElement (doc, root);
  attribute (root, "type", "dir");
  element (root, "name", "pirwa");
  element (element (root, "target", NULL), "path", path);
  return serialize (doc);
}

char *
pirwa_config_domain (const PirwaCreate *o, const char *uuid, const char *disk_path,
                     const char *pool_uuid, const char *volume_key, gboolean uefi, gboolean passt,
                     const char *os_id)
{
  xmlDoc *doc = xmlNewDoc (BAD_CAST "1.0");
  xmlNode *root = xmlNewNode (NULL, BAD_CAST "domain");
  xmlDocSetRootElement (doc, root);
  attribute (root, "type", "kvm");
  element (root, "name", o->name);
  element (root, "uuid", uuid);
  g_autofree char *ram = g_strdup_printf ("%u", o->memory_mb);
  g_autofree char *cpus = g_strdup_printf ("%u", o->cpus);
  attribute (element (root, "memory", ram), "unit", "MiB");
  attribute (element (root, "currentMemory", ram), "unit", "MiB");
  element (root, "vcpu", cpus);
  xmlNode *cpu = element (root, "cpu", NULL);
  attribute (cpu, "mode", "host-passthrough");
  xmlNode *topology = element (cpu, "topology", NULL);
  attribute (topology, "sockets", "1");
  attribute (topology, "cores", cpus);
  attribute (topology, "threads", "1");
  xmlNode *metadata = element (root, "metadata", NULL);
  xmlNode *managed = element (metadata, "managed", NULL);
  xmlNs *ns = xmlNewNs (managed, BAD_CAST PIRWA_METADATA_NS, BAD_CAST "pirwa");
  xmlSetNs (managed, ns);
  attribute (managed, "version", "1");
  xmlNode *volume = xmlNewChild (managed, ns, BAD_CAST "volume", NULL);
  attribute (volume, "pool", pool_uuid);
  attribute (volume, "key", volume_key);
  g_autofree char *volume_name = g_strdup_printf ("pirwa-%s.qcow2", uuid);
  attribute (volume, "name", volume_name);
  if (os_id)
    {
      xmlNode *info = element (metadata, "libosinfo", NULL);
      xmlNs *osns = xmlNewNs (info, BAD_CAST PIRWA_OSINFO_NS, BAD_CAST "libosinfo");
      xmlSetNs (info, osns);
      attribute (xmlNewChild (info, osns, BAD_CAST "os", NULL), "id", os_id);
    }
  xmlNode *os = element (root, "os", NULL);
  if (uefi)
    attribute (os, "firmware", "efi");
  if (o->guest == PIRWA_GUEST_WINDOWS_11)
    {
      attribute (os, "firmware", "efi");
      xmlNode *firmware = element (os, "firmware", NULL);
      const char *names[] = { "secure-boot", "enrolled-keys" };
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        {
          xmlNode *feature = element (firmware, "feature", NULL);
          attribute (feature, "name", names[i]);
          attribute (feature, "enabled", "yes");
        }
    }
  else if (uefi && o->guest == PIRWA_GUEST_LINUX)
    {
      /* Empty keys allow unsigned installers with either plain or Secure
       * Boot-capable firmware. Leave that capability to libvirt's selection. */
      xmlNode *firmware = element (os, "firmware", NULL);
      xmlNode *feature = element (firmware, "feature", NULL);
      attribute (feature, "name", "enrolled-keys");
      attribute (feature, "enabled", "no");
    }
  xmlNode *type = element (os, "type", "hvm");
  attribute (type, "arch", "x86_64");
  attribute (type, "machine", "q35");
  attribute (element (os, "boot", NULL), "dev", "hd");
  if (!o->import_image)
    attribute (element (os, "boot", NULL), "dev", "cdrom");
  attribute (element (os, "bootmenu", NULL), "enable", "yes");
  xmlNode *features = element (root, "features", NULL);
  element (features, "acpi", NULL);
  element (features, "apic", NULL);
  if (o->guest == PIRWA_GUEST_WINDOWS_11)
    attribute (element (features, "smm", NULL), "state", "on");
  if (o->guest != PIRWA_GUEST_LINUX)
    attribute (element (root, "clock", NULL), "offset", "localtime");
  element (root, "on_poweroff", "destroy");
  element (root, "on_reboot", "restart");
  element (root, "on_crash", "destroy");
  xmlNode *devices = element (root, "devices", NULL);
  xmlNode *disk = element (devices, "disk", NULL);
  attribute (disk, "type", "file");
  attribute (disk, "device", "disk");
  xmlNode *driver = element (disk, "driver", NULL);
  attribute (driver, "name", "qemu");
  attribute (driver, "type", "qcow2");
  attribute (element (disk, "source", NULL), "file", disk_path);
  xmlNode *target = element (disk, "target", NULL);
  attribute (target, "dev", "vda");
  attribute (target, "bus", "virtio");
  if (!o->import_image)
    {
      disk = element (devices, "disk", NULL);
      attribute (disk, "type", "file");
      attribute (disk, "device", "cdrom");
      driver = element (disk, "driver", NULL);
      attribute (driver, "name", "qemu");
      attribute (driver, "type", "raw");
      attribute (element (disk, "source", NULL), "file", o->source);
      target = element (disk, "target", NULL);
      attribute (target, "dev", "sda");
      attribute (target, "bus", "sata");
      element (disk, "readonly", NULL);
    }
  if (o->guest != PIRWA_GUEST_LINUX && o->driver_iso)
    {
      disk = element (devices, "disk", NULL);
      attribute (disk, "type", "file");
      attribute (disk, "device", "cdrom");
      driver = element (disk, "driver", NULL);
      attribute (driver, "name", "qemu");
      attribute (driver, "type", "raw");
      attribute (element (disk, "source", NULL), "file", o->driver_iso);
      target = element (disk, "target", NULL);
      attribute (target, "dev", "sdb");
      attribute (target, "bus", "sata");
      element (disk, "readonly", NULL);
    }
  if (o->guest == PIRWA_GUEST_WINDOWS_11)
    {
      xmlNode *tpm = element (devices, "tpm", NULL);
      attribute (tpm, "model", "tpm-crb");
      xmlNode *backend = element (tpm, "backend", NULL);
      attribute (backend, "type", "emulator");
      attribute (backend, "version", "2.0");
      attribute (backend, "persistent_state", "yes");
    }
  xmlNode *nic = element (devices, "interface", NULL);
  attribute (nic, "type", "user");
  attribute (element (nic, "model", NULL), "type", "virtio");
  if (passt)
    attribute (element (nic, "backend", NULL), "type", "passt");
  xmlNode *graphics = element (devices, "graphics", NULL);
  attribute (graphics, "type", "spice");
  attribute (element (graphics, "listen", NULL), "type", "socket");
  /* Non-accelerated virtio-gpu cannot boot some Wayland compositors (COSMIC).
   * QXL supports software rendering over the native, non-GL SPICE console. */
  attribute (element (element (devices, "video", NULL), "model", NULL), "type", "qxl");
  xmlNode *channel = element (devices, "channel", NULL);
  attribute (channel, "type", "spicevmc");
  target = element (channel, "target", NULL);
  attribute (target, "type", "virtio");
  attribute (target, "name", "com.redhat.spice.0");
  xmlNode *sound = element (devices, "sound", NULL);
  attribute (sound, "model", "ich9");
  attribute (element (sound, "audio", NULL), "id", "1");
  xmlNode *audio = element (devices, "audio", NULL);
  attribute (audio, "id", "1");
  attribute (audio, "type", "spice");
  xmlNode *input = element (devices, "input", NULL);
  attribute (input, "type", "tablet");
  attribute (input, "bus", "usb");
  xmlNode *controller = element (devices, "controller", NULL);
  attribute (controller, "type", "usb");
  attribute (controller, "model", "qemu-xhci");
  attribute (element (devices, "memballoon", NULL), "model", "virtio");
  return serialize (doc);
}

static void
parse_media (xmlDoc *doc, PirwaDetails *details)
{
  xmlXPathObject *disks = query (doc, "/domain/devices/disk[@device='cdrom'][target/@dev]"
                                      "[source/@file!='' or source/@dev!='' or source/@name!=''"
                                      " or source/@volume!='']");
  if (disks && disks->nodesetval && disks->nodesetval->nodeNr > 0)
    {
      xmlNode *disk = disks->nodesetval->nodeTab[0];
      xmlDoc *eject = xmlNewDoc (BAD_CAST "1.0");
      xmlNode *copy = xmlDocCopyNode (disk, eject, 1);
      xmlDocSetRootElement (eject, copy);
      for (xmlNode *child = copy->children; child;)
        {
          xmlNode *next = child->next;
          if (xmlStrEqual (child->name, BAD_CAST "source"))
            {
              const char *attributes[] = { "file", "dev", "name", "volume" };
              for (guint i = 0; i < G_N_ELEMENTS (attributes); i++)
                {
                  xmlChar *source = xmlGetProp (child, BAD_CAST attributes[i]);
                  if (source && *source && !details->media_source)
                    details->media_source = g_strdup ((const char *)source);
                  xmlFree (source);
                }
              /* Removing the source requests ejection. Preserve target attributes;
               * QEMU rejects an explicit change to the tray during a live update. */
              xmlUnlinkNode (child);
              xmlFreeNode (child);
            }
          else if (xmlStrEqual (child->name, BAD_CAST "target"))
            {
              xmlChar *target = xmlGetProp (child, BAD_CAST "dev");
              details->media_target = g_strdup ((const char *)target);
              xmlFree (target);
            }
          child = next;
        }
      details->media_eject_xml = serialize (eject);
    }
  if (disks)
    xmlXPathFreeObject (disks);
}

PirwaDetails *
pirwa_config_parse (const char *xml, GError **error)
{
  xmlDoc *doc = parse (xml, error);
  if (!doc)
    return NULL;
  PirwaDetails *details = g_new0 (PirwaDetails, 1);
  details->name = value (doc, "/domain/name");
  details->disk_path = value (doc, "/domain/devices/disk[@device='disk']/source/@file");
  details->pool_uuid = value (doc, "/domain/metadata/p:managed[@version='1']/p:volume/@pool");
  details->volume_key = value (doc, "/domain/metadata/p:managed[@version='1']/p:volume/@key");
  details->volume_name = value (doc, "/domain/metadata/p:managed[@version='1']/p:volume/@name");
  g_autofree char *cpus = value (doc, "/domain/vcpu");
  g_autofree char *memory = value (doc, "/domain/memory");
  g_autofree char *unit = value (doc, "/domain/memory/@unit");
  details->cpus = cpus ? g_ascii_strtoull (cpus, NULL, 10) : 1;
  guint64 ram = memory ? g_ascii_strtoull (memory, NULL, 10) : 0;
  if (g_strcmp0 (unit, "MiB") == 0)
    details->memory_mb = ram;
  else if (g_strcmp0 (unit, "GiB") == 0)
    details->memory_mb = ram * 1024;
  else if (g_strcmp0 (unit, "bytes") == 0 || g_strcmp0 (unit, "B") == 0)
    details->memory_mb = ram / (1024 * 1024);
  else
    details->memory_mb = ram / 1024;
  xmlXPathObject *spice
      = query (doc, "/domain/devices/graphics[@type='spice'] and "
                    "not(/domain/devices/graphics[@type='spice']/gl[@enable='yes'])");
  details->native_console = spice && xmlXPathCastToBoolean (spice);
  if (spice)
    xmlXPathFreeObject (spice);
  parse_media (doc, details);
  details->has_cdrom = details->media_source != NULL;
  xmlXPathObject *nvram = query (doc, "/domain/os/nvram | /domain/os/varstore");
  details->has_nvram = nvram && xmlXPathCastToBoolean (nvram);
  if (nvram)
    xmlXPathFreeObject (nvram);
  xmlXPathObject *tpm = query (doc, "/domain/devices/tpm/backend[@type='emulator']");
  details->has_tpm = tpm && xmlXPathCastToBoolean (tpm);
  if (tpm)
    xmlXPathFreeObject (tpm);
  details->managed
      = details->pool_uuid && details->volume_key && details->volume_name && details->disk_path;
  xmlFreeDoc (doc);
  return details;
}

char *
pirwa_config_edit (const char *xml, const char *name, guint cpus, guint memory_mb, gboolean eject,
                   GError **error)
{
  PirwaCreate o = { .name = (char *)name, .cpus = cpus, .memory_mb = memory_mb, .disk_gb = 4 };
  if (!pirwa_config_validate (&o, error))
    return NULL;
  xmlDoc *doc = parse (xml, error);
  if (!doc)
    return NULL;
  const char *paths[] = { "/domain/name", "/domain/vcpu", "/domain/memory", "/domain/currentMemory",
                          "/domain/maxMemory" };
  g_autofree char *ram = g_strdup_printf ("%u", memory_mb);
  g_autofree char *count = g_strdup_printf ("%u", cpus);
  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      xmlXPathObject *result = query (doc, paths[i]);
      if (result && result->nodesetval)
        for (int j = 0; j < result->nodesetval->nodeNr; j++)
          {
            xmlNode *node = result->nodesetval->nodeTab[j];
            xmlNodeSetContent (node, NULL);
            xmlAddChild (node, xmlNewText (BAD_CAST (i == 0 ? name : i == 1 ? count : ram)));
            if (i >= 2)
              attribute (node, "unit", "MiB");
            if (i == 1)
              xmlUnsetProp (node, BAD_CAST "current");
          }
      if (result)
        xmlXPathFreeObject (result);
    }
  if (eject)
    {
      xmlXPathObject *result = query (
          doc, "/domain/devices/disk[@device='cdrom']/source | /domain/os/boot[@dev='cdrom']");
      if (result && result->nodesetval)
        for (int i = 0; i < result->nodesetval->nodeNr; i++)
          {
            xmlNode *node = result->nodesetval->nodeTab[i];
            xmlUnlinkNode (node);
            xmlFreeNode (node);
          }
      if (result)
        xmlXPathFreeObject (result);
    }
  xmlXPathObject *topologies = query (doc, "/domain/cpu/topology");
  if (topologies && topologies->nodesetval)
    for (int i = 0; i < topologies->nodesetval->nodeNr; i++)
      {
        xmlNode *topology = topologies->nodesetval->nodeTab[i];
        attribute (topology, "sockets", "1");
        attribute (topology, "cores", count);
        attribute (topology, "threads", "1");
        xmlUnsetProp (topology, BAD_CAST "dies");
        xmlUnsetProp (topology, BAD_CAST "clusters");
      }
  if (topologies)
    xmlXPathFreeObject (topologies);
  return serialize (doc);
}

void
pirwa_details_free (PirwaDetails *details)
{
  if (!details)
    return;
  g_free (details->name);
  g_free (details->disk_path);
  g_free (details->pool_uuid);
  g_free (details->volume_key);
  g_free (details->volume_name);
  g_free (details->media_source);
  g_free (details->media_target);
  g_free (details->media_eject_xml);
  g_free (details);
}

gboolean
pirwa_qcow2_inspect (int fd, guint64 *capacity, GError **error)
{
  guint8 header[104];
  struct stat st;
  if (fstat (fd, &st) < 0 || !S_ISREG (st.st_mode)
      || pread (fd, header, sizeof header, 0) != sizeof header)
    goto invalid;
  guint32 version, cluster_bits, encryption;
  guint64 backing, size, incompatible = 0;
  memcpy (&version, header + 4, 4);
  version = GUINT32_FROM_BE (version);
  memcpy (&backing, header + 8, 8);
  backing = GUINT64_FROM_BE (backing);
  memcpy (&cluster_bits, header + 20, 4);
  cluster_bits = GUINT32_FROM_BE (cluster_bits);
  memcpy (&size, header + 24, 8);
  size = GUINT64_FROM_BE (size);
  memcpy (&encryption, header + 32, 4);
  encryption = GUINT32_FROM_BE (encryption);
  if (version == 3)
    {
      memcpy (&incompatible, header + 72, 8);
      incompatible = GUINT64_FROM_BE (incompatible);
    }
  if (memcmp (header, "QFI\373", 4) || (version != 2 && version != 3) || backing || encryption
      || incompatible || cluster_bits < 9 || cluster_bits > 21 || size == 0
      || size > (2048ULL << 30))
    goto invalid;
  *capacity = size;
  return TRUE;
invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Import requires a clean, standalone, unencrypted qcow2 image (up to 2 "
                       "TiB). Backing chains and external data files are unsupported.");
  return FALSE;
}
