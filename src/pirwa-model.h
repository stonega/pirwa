/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-machine.h"
#include <gio/gio.h>

/* Main-thread reconciliation. Reject late snapshots and preserve item identity. */
gboolean pirwa_model_apply (GListStore *store, guint64 *last_serial, guint64 serial,
                            GPtrArray *snapshots);
