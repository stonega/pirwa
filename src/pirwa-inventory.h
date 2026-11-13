/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pirwa-machine.h"
#include <gio/gio.h>

G_BEGIN_DECLS

#define PIRWA_DEFAULT_URI "qemu:///session"

/* Callback runs on the initiating main context. Result owns its VM snapshots. */
void pirwa_inventory_load_async (const char *uri, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
GPtrArray *pirwa_inventory_load_finish (GAsyncResult *result, GError **error);

G_END_DECLS
