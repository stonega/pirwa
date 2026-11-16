/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pirwa-config.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define PIRWA_TYPE_CREATE_DIALOG (pirwa_create_dialog_get_type ())
G_DECLARE_FINAL_TYPE (PirwaCreateDialog, pirwa_create_dialog, PIRWA, CREATE_DIALOG, AdwDialog)

/* create-requested supplies a PirwaCreate valid for the duration of the signal.
 * cancel-requested asks the caller to cancel an in-flight creation. */
AdwDialog *pirwa_create_dialog_new (const char *const *pools, gboolean session);
const char *pirwa_create_dialog_get_avatar (PirwaCreateDialog *self);

G_END_DECLS
