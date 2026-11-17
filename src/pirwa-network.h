/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-usage.h"
#include <gio/gio.h>
#include <libvirt/libvirt.h>

/* Worker-only. Uses the ordinary start unless Session proxy routing is active. */
gboolean pirwa_network_start (virConnectPtr connection, virDomainPtr domain,
                              GCancellable *cancellable, GError **error);
/* Runtime XML only; the persistent definition is never rewritten. */
char *pirwa_network_domain (const char *xml, guint16 port, GError **error);
/* Optional worker-only fallback for the local Session proxy's live interface. */
gboolean pirwa_network_read_usage (virDomainPtr domain, PirwaUsage *usage);
