/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-inventory.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

/* Internal backend interface. Never include this header in GTK code. */
GPtrArray *pirwa_inventory_read (virConnectPtr connection, GCancellable *cancellable,
                                 GError **error);
void pirwa_libvirt_error (GError **error, const char *operation);
