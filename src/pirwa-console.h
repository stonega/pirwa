/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <adwaita.h>

#define PIRWA_TYPE_CONSOLE (pirwa_console_get_type ())
G_DECLARE_FINAL_TYPE (PirwaConsole, pirwa_console, PIRWA, CONSOLE_WINDOW, AdwWindow)
void pirwa_console_present (GtkApplication *application, const char *uri, const char *uuid,
                            const char *name);
