/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
int pirwa_config_spice_index (const char *xml);
#include <gio/gio.h>

#define PIRWA_METADATA_NS "urn:pirwa:domain:1"
#define PIRWA_OSINFO_NS "http://libosinfo.org/xmlns/libvirt/domain/1.0"

typedef enum
{
  PIRWA_GUEST_LINUX,
  PIRWA_GUEST_WINDOWS_10,
  PIRWA_GUEST_WINDOWS_11
} PirwaGuest;

typedef struct
{
  char *name;
  char *source;
  char *pool;
  guint cpus;
  guint memory_mb;
  guint disk_gb;
  gboolean import_image;
  gboolean legacy_bios;
  PirwaGuest guest;
  char *driver_iso;
} PirwaCreate;

typedef struct
{
  char *name;
  char *disk_path;
  char *pool_uuid;
  char *volume_key;
  char *volume_name;
  char *media_source;
  char *media_target;
  char *media_eject_xml;
  guint cpus;
  guint memory_mb;
  gboolean native_console;
  gboolean has_cdrom;
  gboolean has_nvram;
  gboolean has_tpm;
  gboolean managed;
} PirwaDetails;

gboolean pirwa_config_validate (const PirwaCreate *options, GError **error);
gboolean pirwa_config_check_host (const PirwaCreate *options, const char *caps, GError **error);
char *pirwa_config_domain (const PirwaCreate *options, const char *uuid, const char *disk_path,
                           const char *pool_uuid, const char *volume_key, gboolean uefi,
                           gboolean passt, const char *os_id);
char *pirwa_config_volume (const char *uuid, guint64 capacity);
char *pirwa_config_pool (const char *path);
char *pirwa_config_os_id (const char *xml);
PirwaDetails *pirwa_config_parse (const char *xml, GError **error);
char *pirwa_config_edit (const char *xml, const char *name, guint cpus, guint memory_mb,
                         gboolean eject, GError **error);
void pirwa_details_free (PirwaDetails *details);
gboolean pirwa_qcow2_inspect (int fd, guint64 *capacity, GError **error);
gboolean pirwa_config_supports (const char *xml, const char *expression);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaDetails, pirwa_details_free)
