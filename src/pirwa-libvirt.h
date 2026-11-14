/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-inventory.h"
#include "pirwa-usage.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

/* Internal backend interface. Never include this header in GTK code. */
typedef struct _PirwaInventoryCache PirwaInventoryCache;
PirwaInventoryCache *pirwa_inventory_cache_new (void);
void pirwa_inventory_cache_free (PirwaInventoryCache *cache);
void pirwa_inventory_cache_clear (PirwaInventoryCache *cache);
GPtrArray *pirwa_inventory_read_cached (virConnectPtr connection, PirwaInventoryCache *cache,
                                        GCancellable *cancellable, GError **error);
GPtrArray *pirwa_inventory_read (virConnectPtr connection, GCancellable *cancellable,
                                 GError **error);
void pirwa_libvirt_error (GError **error, const char *operation);
gboolean pirwa_usage_read (virDomainPtr domain, PirwaUsage *usage, GError **error);
void pirwa_usage_from_stats (const virDomainStatsRecord *record, gint64 sampled_at_us,
                             PirwaUsage *usage);
