/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define PIRWA_TYPE_MACHINE (pirwa_machine_get_type ())
G_DECLARE_FINAL_TYPE (PirwaMachine, pirwa_machine, PIRWA, MACHINE, GObject)

/* Workers return snapshots. GTK may update canonical copies on the main thread.
 * No libvirt handles cross into this model. */
PirwaMachine *pirwa_machine_new (const char *uuid, const char *name, const char *state);
const char *pirwa_machine_get_uuid (PirwaMachine *self);
const char *pirwa_machine_get_name (PirwaMachine *self);
const char *pirwa_machine_get_title (PirwaMachine *self);
void pirwa_machine_set_os_name (PirwaMachine *self, const char *os_name);
const char *pirwa_machine_get_state (PirwaMachine *self);
PirwaMachine *pirwa_machine_new_full (const char *uuid, const char *name, const char *state,
                                      int state_code);
int pirwa_machine_get_state_code (PirwaMachine *self);
void pirwa_machine_update (PirwaMachine *self, PirwaMachine *snapshot);

G_END_DECLS
