/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

typedef struct
{
  char *default_uri;
  char *https_uri;
  char *dns;
  char **ignore_hosts;
} PirwaProxy;

/* NULL without an error means the desktop has no proxy configured. */
PirwaProxy *pirwa_proxy_read (GError **error);
char *pirwa_proxy_serialize (const PirwaProxy *proxy);
PirwaProxy *pirwa_proxy_parse (const char *data, GError **error);
GSocketConnection *pirwa_proxy_connect (const PirwaProxy *proxy, const char *host, guint16 port,
                                        GCancellable *cancellable, GError **error);
void pirwa_proxy_free (PirwaProxy *proxy);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaProxy, pirwa_proxy_free)
