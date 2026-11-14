/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include <spice-client.h>

#define PIRWA_TYPE_DISPLAY (pirwa_display_get_type ())
G_DECLARE_FINAL_TYPE (PirwaDisplay, pirwa_display, PIRWA, DISPLAY, GtkWidget)

GtkWidget *pirwa_display_new (void);
/* Primary memory stays owned by the transport. Call clear before it is freed.
 * These functions, like GTK, must be used on the main thread. */
gboolean pirwa_display_set_primary (PirwaDisplay *self, const SpiceDisplayPrimary *primary);
void pirwa_display_invalidate (PirwaDisplay *self, int x, int y, int width, int height);
void pirwa_display_clear (PirwaDisplay *self);
