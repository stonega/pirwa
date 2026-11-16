/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

GSettings *pirwa_preferences_new (void);
char *pirwa_preferences_get_selected_machine (GSettings *settings, const char *uri);
gboolean pirwa_preferences_set_selected_machine (GSettings *settings, const char *uri,
                                                 const char *uuid);
char *pirwa_preferences_get_machine_avatar (GSettings *settings, const char *uri, const char *uuid);
gboolean pirwa_preferences_set_machine_avatar (GSettings *settings, const char *uri,
                                               const char *uuid, const char *preset);
