/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PIRWA_TYPE_WINDOW (pirwa_window_get_type ())
G_DECLARE_FINAL_TYPE (PirwaWindow, pirwa_window, PIRWA, WINDOW, AdwApplicationWindow)

GtkWindow *pirwa_window_new (GtkApplication *application, const char *uri);

G_END_DECLS
