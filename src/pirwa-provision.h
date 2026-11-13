/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-config.h"
#include "pirwa-libvirt.h"

typedef void (*PirwaProgress) (double fraction, const char *stage, gpointer user_data);
char *pirwa_provision_create (virConnectPtr connection, const PirwaCreate *options,
                              GCancellable *cancellable, PirwaProgress progress, gpointer user_data,
                              GError **error);
gboolean pirwa_provision_owned (virConnectPtr connection, const char *uuid, PirwaDetails *details);
gboolean pirwa_provision_remove (virConnectPtr connection, virDomainPtr domain, const char *uuid,
                                 gboolean delete_disk, GCancellable *cancellable, GError **error);
/* Internal recovery helpers; usable with the built-in test driver. */
gboolean pirwa_provision_rollback (virConnectPtr connection, virDomainPtr domain,
                                   virStorageVolPtr volume, GError **error);
gboolean pirwa_provision_references (const char *xml, const char *path, const char *name);
