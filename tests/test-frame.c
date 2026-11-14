/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-frame.h"
#include <linux/input-event-codes.h>

static void
test_owned_pixels (void)
{
  guint8 source[] = { 0, 0, 255, 0, 0, 255, 0, 0, 99, 99, 99, 99 };
  SpiceDisplayPrimary primary = {
    .format = SPICE_SURFACE_FMT_32_xRGB, .width = 2, .height = 1, .stride = 12, .data = source
  };
  g_autoptr (GdkTexture) first = pirwa_frame_texture (&primary, NULL, NULL);
  g_assert_nonnull (first);
  memset (source, 0, sizeof source);
  guint8 pixels[8];
  gdk_texture_download (first, pixels, 8);
  const guint8 expected[] = { 0, 0, 255, 255, 0, 255, 0, 255 };
  g_assert_cmpmem (pixels, sizeof pixels, expected, sizeof expected);
  source[0] = 255;
  cairo_rectangle_int_t rect = { 0, 0, 1, 1 };
  cairo_region_t *damage = cairo_region_create_rectangle (&rect);
  g_autoptr (GdkTexture) second = pirwa_frame_texture (&primary, first, damage);
  g_assert_nonnull (second);
  cairo_region_destroy (damage);
  gdk_texture_download (second, pixels, 8);
  g_assert_cmpuint (pixels[0], ==, 255);
  /* A render thread retaining the old texture still sees the old frame. */
  gdk_texture_download (first, pixels, 8);
  g_assert_cmpmem (pixels, sizeof pixels, expected, sizeof expected);
}

static void
test_rgb16_and_validation (void)
{
  guint16 source[] = { 0xf800, 0x07e0, 0x001f };
  SpiceDisplayPrimary primary = { .format = SPICE_SURFACE_FMT_16_565,
                                  .width = 3,
                                  .height = 1,
                                  .stride = 6,
                                  .data = (guint8 *)source };
  g_autoptr (GdkTexture) texture = pirwa_frame_texture (&primary, NULL, NULL);
  guint8 pixels[12];
  gdk_texture_download (texture, pixels, sizeof pixels);
  const guint8 expected[] = { 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255 };
  g_assert_cmpmem (pixels, sizeof pixels, expected, sizeof expected);
  source[0] = 0x7c00;
  source[1] = 0x03e0;
  primary.format = SPICE_SURFACE_FMT_16_555;
  g_autoptr (GdkTexture) rgb555 = pirwa_frame_texture (&primary, NULL, NULL);
  gdk_texture_download (rgb555, pixels, sizeof pixels);
  g_assert_cmpmem (pixels, sizeof pixels, expected, sizeof expected);
  primary.stride = 5;
  g_assert_null (pirwa_frame_texture (&primary, NULL, NULL));
  primary.stride = 6;
  primary.height = 8193;
  g_assert_null (pirwa_frame_texture (&primary, NULL, NULL));
  primary.height = 1;
  primary.format = SPICE_SURFACE_FMT_8_A;
  g_assert_null (pirwa_frame_texture (&primary, NULL, NULL));
  primary.data = NULL;
  g_assert_null (pirwa_frame_texture (&primary, NULL, NULL));
}

static void
test_alpha (void)
{
  guint8 pixel[] = { 255, 0, 0, 128 };
  SpiceDisplayPrimary primary = {
    .format = SPICE_SURFACE_FMT_32_ARGB, .width = 1, .height = 1, .stride = 4, .data = pixel
  };
  g_autoptr (GdkTexture) texture = pirwa_frame_texture (&primary, NULL, NULL);
  guint8 output[4];
  gdk_texture_download (texture, output, 4);
  const guint8 expected[] = { 128, 0, 0, 128 };
  g_assert_cmpmem (output, sizeof output, expected, sizeof expected);
}

static void
test_input_coordinates (void)
{
  int x, y;
  g_assert_false (pirwa_frame_position (1000, 1000, 1920, 1080, 500, 10, &x, &y));
  g_assert_true (pirwa_frame_position (1000, 1000, 1920, 1080, 500, 500, &x, &y));
  g_assert_cmpint (x, ==, 960);
  g_assert_cmpint (y, ==, 540);
  g_assert_false (pirwa_frame_position (0, 0, 1920, 1080, 0, 0, &x, &y));
  g_assert_false (pirwa_frame_position (1000, 1000, 0, 0, 0, 0, &x, &y));
  g_assert_true (pirwa_frame_position (100, 100, 100, 100, 99, 99, &x, &y));
  g_assert_cmpint (x, ==, 99);
  g_assert_false (pirwa_frame_position (100, 100, 100, 100, 100, 99, &x, &y));
  g_assert_cmpuint (pirwa_frame_scancode (KEY_A + 8), ==, 0x1e);
  g_assert_cmpuint (pirwa_frame_scancode (KEY_RIGHTCTRL + 8), ==, 0x11d);
  g_assert_cmpuint (pirwa_frame_scancode (KEY_UP + 8), ==, 0x148);
  g_assert_cmpuint (pirwa_frame_scancode (KEY_LEFTMETA + 8), ==, 0x15b);
  g_assert_cmpuint (pirwa_frame_scancode (1), ==, 0);
  g_assert_cmpuint (pirwa_frame_scancode (10000), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/console/owned-pixels-and-damage", test_owned_pixels);
  g_test_add_func ("/console/formats-and-bounds", test_rgb16_and_validation);
  g_test_add_func ("/console/input-mapping", test_input_coordinates);
  g_test_add_func ("/console/alpha", test_alpha);
  return g_test_run ();
}
