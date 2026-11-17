/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-import.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <sys/stat.h>
#include <unistd.h>

static xmlXPathObject *
query (xmlDoc *doc, const char *expression)
{
  xmlXPathContext *context = xmlXPathNewContext (doc);
  xmlXPathObject *result = xmlXPathEvalExpression (BAD_CAST expression, context);
  xmlXPathFreeContext (context);
  return result;
}

static xmlNode *
node (xmlDoc *doc, const char *expression)
{
  xmlXPathObject *result = query (doc, expression);
  xmlNode *first = result && result->nodesetval && result->nodesetval->nodeNr
                       ? result->nodesetval->nodeTab[0]
                       : NULL;
  if (result)
    xmlXPathFreeObject (result);
  return first;
}

static char *
content (xmlDoc *doc, const char *expression)
{
  xmlChar *value = xmlNodeGetContent (node (doc, expression));
  char *copy = value && *value ? g_strdup ((const char *)value) : NULL;
  xmlFree (value);
  return copy;
}

static void
remove_nodes (xmlDoc *doc, const char *expression)
{
  xmlXPathObject *result = query (doc, expression);
  if (result && result->nodesetval)
    for (int i = 0; i < result->nodesetval->nodeNr; i++)
      {
        xmlNode *item = result->nodesetval->nodeTab[i];
        xmlUnlinkNode (item);
        xmlFreeNode (item);
      }
  if (result)
    xmlXPathFreeObject (result);
}

static char *
serialize (xmlDoc *doc)
{
  xmlChar *buffer;
  int size;
  xmlDocDumpMemory (doc, &buffer, &size);
  char *copy = g_strndup ((const char *)buffer, size);
  xmlFree (buffer);
  return copy;
}

void
pirwa_import_free (PirwaImport *plan)
{
  if (!plan)
    return;
  g_free (plan->xml);
  g_free (plan->disk);
  g_free (plan->loader);
  g_free (plan->nvram);
  g_free (plan->uuid);
  g_free (plan);
}

PirwaImport *
pirwa_import_prepare (const char *xml, GError **error)
{
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), "boxes.xml", NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc || doc->intSubset || doc->extSubset)
    {
      if (doc)
        xmlFreeDoc (doc);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Boxes definition");
      return NULL;
    }
  /* Reject dependencies that cannot be independently copied by this importer. */
  const char *supported
      = "/domain[@type='kvm' or @type='qemu' or @type='test']"
        "[os/type='hvm'][name][uuid]"
        "[count(devices/disk[@device='disk'])=1]"
        "[devices/disk[@device='disk'][@type='file'][driver/@type='qcow2'][source/@file]]"
        "[not(devices/disk[not(@device='disk' or @device='cdrom')])]"
        "[not(devices/disk/backingStore[source] | devices/disk/dataStore | devices/disk/auth"
        " | devices/disk/encryption | devices/disk[@device='disk']/readonly | "
        "devices/disk/shareable)]"
        "[not(devices/tpm | devices/hostdev | devices/filesystem | devices/shmem"
        " | devices/memory | devices/vsock | devices/redirdev/source | devices/crypto"
        " | devices/interface[not(@type='user')] | devices/interface/source"
        " | devices/interface/filterref | devices/interface/script"
        " | devices/rng/backend[not(@model='random') or normalize-space(.)!='/dev/urandom']"
        " | devices/smartcard | devices/lease | devices/pstore | devices/iommu"
        " | devices/input/source | devices/*/log | devices/parallel"
        " | devices/audio[not(@type='spice' or @type='none')]"
        " | devices/graphics[not(@type='spice')] | devices/interface/portForward"
        " | os/kernel | os/initrd | os/dtb | os/varstore | launchSecurity"
        " | /domain/bootloader | /domain/sysinfo//entry[@file]"
        " | /domain/*[namespace-uri()!=''])]"
        "[not(devices/channel[not(@type='spicevmc' or @type='unix')])]"
        "[not(devices/channel[@type='unix']/source[@mode!='bind'])]"
        "[not(devices/serial[not(@type='pty')] | devices/console[not(@type='pty')])]"
        "[not(os/loader[@type!='pflash'] | os/nvram[@type and @type!='file'])]";
  if (!node (doc, supported))
    {
      xmlFreeDoc (doc);
      g_set_error_literal (
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "Import supports one standalone qcow2 disk with user networking. Extra disks, backing "
          "chains, TPM, shared folders, passthrough and custom host devices are not supported.");
      return NULL;
    }
  g_autoptr (PirwaImport) plan = g_new0 (PirwaImport, 1);
  plan->disk = content (doc, "/domain/devices/disk[@device='disk']/source/@file");
  plan->loader = content (doc, "/domain/os/loader");
  plan->nvram = content (doc, "/domain/os/nvram/source/@file");
  if (!plan->nvram)
    plan->nvram = content (doc, "/domain/os/nvram[not(source)]");
  if (!g_path_is_absolute (plan->disk) || (!!plan->loader != !!plan->nvram)
      || (plan->nvram && (!g_path_is_absolute (plan->nvram) || !g_path_is_absolute (plan->loader))))
    {
      xmlFreeDoc (doc);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "The machine must have an absolute disk path and a complete UEFI "
                           "loader/NVRAM pair, or use legacy BIOS.");
      return NULL;
    }
  plan->uuid = g_uuid_string_random ();
  xmlNodeSetContent (node (doc, "/domain/uuid"), BAD_CAST plan->uuid);
  g_autofree char *name = content (doc, "/domain/title");
  if (!name)
    name = content (doc, "/domain/name");
  g_autofree char *title = g_strdup_printf ("%s (Imported %.8s)", name, plan->uuid);
  g_autofree char *internal_name = g_strdup_printf ("pirwa-%s", plan->uuid);
  xmlNodeSetContent (node (doc, "/domain/name"), BAD_CAST internal_name);
  remove_nodes (doc, "/domain/title");
  xmlNewTextChild (xmlDocGetRootElement (doc), NULL, BAD_CAST "title", BAD_CAST title);
  xmlNode *system_uuid = node (doc, "/domain/sysinfo/system/entry[@name='uuid']");
  if (system_uuid)
    xmlNodeSetContent (system_uuid, BAD_CAST plan->uuid);
  remove_nodes (doc, "/domain/genid | /domain/seclabel | /domain/resource"
                     " | /domain/devices/emulator | /domain/devices/interface/mac"
                     " | /domain/devices/interface/target | /domain/devices/interface/backend"
                     " | /domain/devices/channel[@type='unix']/source"
                     " | /domain/devices/serial/source | /domain/devices/console/source"
                     " | /domain/devices/disk[@device='cdrom']/source"
                     " | /domain/metadata/*[namespace-uri()='urn:pirwa:domain:1']");
  /* Let the destination allocate a local display socket, never reuse Boxes' socket. */
  xmlNode *graphics = node (doc, "/domain/devices/graphics[@type='spice']");
  if (graphics)
    {
      remove_nodes (doc, "/domain/devices/graphics[@type='spice']/listen");
      xmlUnsetProp (graphics, BAD_CAST "socket");
      xmlUnsetProp (graphics, BAD_CAST "port");
      xmlUnsetProp (graphics, BAD_CAST "tlsPort");
      xmlUnsetProp (graphics, BAD_CAST "listen");
      xmlUnsetProp (graphics, BAD_CAST "autoport");
      xmlNode *listen = xmlNewChild (graphics, NULL, BAD_CAST "listen", NULL);
      xmlSetProp (listen, BAD_CAST "type", BAD_CAST "socket");
    }
  plan->xml = serialize (doc);
  xmlFreeDoc (doc);
  return g_steal_pointer (&plan);
}

char *
pirwa_import_domain (const PirwaImport *plan, const char *disk, const char *loader,
                     const char *nvram, const char *pool, const char *key)
{
  xmlDoc *doc = xmlReadMemory (plan->xml, strlen (plan->xml), NULL, NULL, XML_PARSE_NONET);
  xmlSetProp (node (doc, "/domain/devices/disk[@device='disk']/source"), BAD_CAST "file",
              BAD_CAST disk);
  if (loader)
    {
      /* Use the exact copied firmware as a manual loader. Auto-selection would
       * try to match our private path against host firmware descriptors. */
      xmlUnsetProp (node (doc, "/domain/os"), BAD_CAST "firmware");
      remove_nodes (doc, "/domain/os/firmware");
      xmlNodeSetContent (node (doc, "/domain/os/loader"), BAD_CAST loader);
      xmlNode *vars = node (doc, "/domain/os/nvram");
      xmlUnsetProp (vars, BAD_CAST "template");
      xmlUnsetProp (vars, BAD_CAST "templateFormat");
      xmlNode *source = node (doc, "/domain/os/nvram/source");
      if (source)
        xmlSetProp (source, BAD_CAST "file", BAD_CAST nvram);
      else
        xmlNodeSetContent (vars, BAD_CAST nvram);
    }
  xmlNode *metadata = node (doc, "/domain/metadata");
  if (!metadata)
    metadata = xmlNewChild (xmlDocGetRootElement (doc), NULL, BAD_CAST "metadata", NULL);
  xmlNode *managed = xmlNewChild (metadata, NULL, BAD_CAST "managed", NULL);
  xmlNs *ns = xmlNewNs (managed, BAD_CAST PIRWA_METADATA_NS, BAD_CAST "pirwa");
  xmlSetNs (managed, ns);
  xmlSetProp (managed, BAD_CAST "version", BAD_CAST "1");
  xmlNode *volume = xmlNewChild (managed, ns, BAD_CAST "volume", NULL);
  xmlSetProp (volume, BAD_CAST "pool", BAD_CAST pool);
  xmlSetProp (volume, BAD_CAST "key", BAD_CAST key);
  g_autofree char *name = g_strdup_printf ("pirwa-%s.qcow2", plan->uuid);
  xmlSetProp (volume, BAD_CAST "name", BAD_CAST name);
  char *result = serialize (doc);
  xmlFreeDoc (doc);
  return result;
}

gboolean
pirwa_import_copy_firmware (const char *source, const char *target, GCancellable *cancellable,
                            GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;
  int input = open (source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct stat before, after;
  if (input < 0 || fstat (input, &before) < 0 || !S_ISREG (before.st_mode) || before.st_size <= 0
      || before.st_size > 64 * 1024 * 1024)
    {
      if (input >= 0)
        close (input);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Firmware must be a readable regular file, up to 64 MiB");
      return FALSE;
    }
  int output = open (target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  gboolean ok = FALSE;
  if (output < 0)
    goto io_error;
  char buffer[65536];
  off_t total = 0;
  while (total < before.st_size)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;
      ssize_t count = read (input, buffer, MIN ((off_t)sizeof buffer, before.st_size - total));
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          errno = EIO;
          goto io_error;
        }
      for (ssize_t done = 0; done < count;)
        {
          ssize_t size = write (output, buffer + done, count - done);
          if (size < 0 && errno == EINTR)
            continue;
          if (size <= 0)
            goto io_error;
          done += size;
        }
      total += count;
    }
  if (fstat (input, &after) < 0 || before.st_size != after.st_size
      || before.st_mtim.tv_sec != after.st_mtim.tv_sec
      || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
      || before.st_ctim.tv_sec != after.st_ctim.tv_sec
      || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Firmware changed during import; shut down the source machine");
      goto out;
    }
  if (fsync (output) < 0)
    goto io_error;
  ok = TRUE;
  goto out;
io_error:
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot copy firmware: %s",
               g_strerror (errno));
out:
  close (input);
  if (output >= 0)
    {
      if (close (output) < 0 && ok)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot finish firmware copy: %s", g_strerror (errno));
          ok = FALSE;
        }
      if (!ok)
        g_unlink (target);
    }
  return ok;
}
