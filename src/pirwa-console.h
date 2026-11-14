/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <adwaita.h>

#define PIRWA_TYPE_CONSOLE (pirwa_console_get_type ())
G_DECLARE_FINAL_TYPE (PirwaConsole, pirwa_console, PIRWA, CONSOLE_VIEW, GtkBox)

/* A live session widget, movable between the content panel and a window. */
PirwaConsole *pirwa_console_new (const char *uri, const char *uuid, const char *name);
gboolean pirwa_console_matches (PirwaConsole *self, const char *uri, const char *uuid);
PirwaConsole *pirwa_console_find_detached (GtkApplication *application, const char *uri,
                                           const char *uuid);
gboolean pirwa_console_is_detached (PirwaConsole *self);
GdkPaintable *pirwa_console_get_preview (PirwaConsole *self);
void pirwa_console_release_input (PirwaConsole *self);
/* The caller must hold a reference and remove the widget from its parent first. */
void pirwa_console_pop_out (PirwaConsole *self, GtkApplication *application);
void pirwa_console_return (PirwaConsole *self);
