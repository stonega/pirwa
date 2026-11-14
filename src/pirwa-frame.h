/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gdk/gdk.h>
#include <spice-client.h>

GdkTexture *pirwa_frame_texture (const SpiceDisplayPrimary *primary, GdkTexture *previous,
                                 cairo_region_t *damage);
gboolean pirwa_frame_position (int width, int height, int guest_width, int guest_height, double x,
                               double y, int *guest_x, int *guest_y);
guint pirwa_frame_scancode (guint hardware_keycode);
