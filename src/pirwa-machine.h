/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define PIRWA_TYPE_MACHINE (pirwa_machine_get_type ())
G_DECLARE_FINAL_TYPE (PirwaMachine, pirwa_machine, PIRWA, MACHINE, GObject)

/* Immutable snapshots: no libvirt handles cross into the GTK layer. */
PirwaMachine *pirwa_machine_new (const char *uuid, const char *name, const char *state);
const char *pirwa_machine_get_uuid (PirwaMachine *self);
const char *pirwa_machine_get_name (PirwaMachine *self);
const char *pirwa_machine_get_state (PirwaMachine *self);

G_END_DECLS
