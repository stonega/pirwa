/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <libvirt/libvirt.h>

/* Worker-only. Uses the ordinary start unless Session proxy routing is active. */
gboolean pirwa_network_start (virConnectPtr connection, virDomainPtr domain,
                              GCancellable *cancellable, GError **error);
/* Runtime XML only; the persistent definition is never rewritten. */
char *pirwa_network_domain (const char *xml, guint16 port, GError **error);
