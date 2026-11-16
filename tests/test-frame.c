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
test_cropped_frame (void)
{
  guint8 pixels[]
      = { 9, 9, 9, 0, 1, 2, 3, 0, 99, 99, 99, 99, 8, 8, 8, 0, 4, 5, 6, 0, 99, 99, 99, 99 };
  SpiceDisplayPrimary primary = {
    .format = SPICE_SURFACE_FMT_32_xRGB, .width = 2, .height = 2, .stride = 12, .data = pixels
  };
  cairo_rectangle_int_t rect = { 1, 0, 1, 2 };
  g_autoptr (GdkTexture) cropped = pirwa_frame_texture_rect (&primary, &rect);
  g_assert_nonnull (cropped);
  memset (pixels, 0, sizeof pixels);
  guint8 actual[8];
  gdk_texture_download (cropped, actual, 4);
  const guint8 expected[] = { 1, 2, 3, 255, 4, 5, 6, 255 };
  g_assert_cmpmem (actual, sizeof actual, expected, sizeof expected);
  rect.x = -1;
  g_assert_null (pirwa_frame_texture_rect (&primary, &rect));
  rect.x = G_MAXINT;
  rect.width = G_MAXINT;
  g_assert_null (pirwa_frame_texture_rect (&primary, &rect));
  rect = (cairo_rectangle_int_t){ 0, 0, 0, 1 };
  g_assert_null (pirwa_frame_texture_rect (&primary, &rect));
  guint16 rgb565[] = { 0xf800, 0x07e0, 0x001f };
  primary = (SpiceDisplayPrimary){ .format = SPICE_SURFACE_FMT_16_565,
                                   .width = 3,
                                   .height = 1,
                                   .stride = 6,
                                   .data = (guint8 *)rgb565 };
  rect = (cairo_rectangle_int_t){ 1, 0, 1, 1 };
  g_autoptr (GdkTexture) converted = pirwa_frame_texture_rect (&primary, &rect);
  gdk_texture_download (converted, actual, 4);
  const guint8 green[] = { 0, 255, 0, 255 };
  g_assert_cmpmem (actual, 4, green, 4);
}

static void
test_monitor_padding (void)
{
  guint32 pixels[32 * 16];
  for (guint i = 0; i < G_N_ELEMENTS (pixels); i++)
    pixels[i] = i;
  SpiceDisplayPrimary primary = { .format = SPICE_SURFACE_FMT_32_xRGB,
                                  .width = 32,
                                  .height = 16,
                                  .stride = 128,
                                  .data = (guint8 *)pixels };
  SpiceDisplayMonitorConfig monitor = { .width = 19, .height = 9 };
  cairo_rectangle_int_t area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.width, ==, 19);
  g_assert_cmpint (area.height, ==, 9);
  g_autoptr (GdkTexture) texture = pirwa_frame_texture_rect (&primary, &area);
  g_assert_cmpint (gdk_texture_get_width (texture), ==, 19);
  g_assert_cmpint (gdk_texture_get_height (texture), ==, 9);
  guint32 output[19 * 9];
  gdk_texture_download (texture, (guint8 *)output, 19 * 4);
  /* The last row comes from the visible monitor, not aligned framebuffer padding. */
  g_assert_cmpuint (output[18 + 8 * 19] & 0xffffff, ==, 18 + 8 * 32);
  monitor.x = 2;
  monitor.y = 3;
  area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.x, ==, 2);
  g_assert_cmpint (area.y, ==, 3);
  g_clear_object (&texture);
  texture = pirwa_frame_texture_rect (&primary, &area);
  gdk_texture_download (texture, (guint8 *)output, 19 * 4);
  g_assert_cmpuint (output[0] & 0xffffff, ==, 2 + 3 * 32);
  g_assert_cmpuint (output[18 + 8 * 19] & 0xffffff, ==, 20 + 11 * 32);
  monitor.x = G_MAXUINT;
  monitor.width = G_MAXUINT;
  area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.width, ==, 32);
  g_assert_cmpint (area.height, ==, 16);
  monitor = (SpiceDisplayMonitorConfig){ .width = 19, .height = 9, .surface_id = 1 };
  area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.width, ==, 32);
  monitor = (SpiceDisplayMonitorConfig){ 0 };
  area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.width, ==, 32);
  primary.data = NULL;
  area = pirwa_frame_monitor_area (&primary, &monitor);
  g_assert_cmpint (area.width, ==, 0);
}

static void
test_contiguous_rgb (void)
{
  for (guint i = 0; i < 2; i++)
    {
      guint8 packed[] = { 1, 2, 3, 128, 4, 5, 6, 128, 7, 8, 9, 128, 10, 11, 12, 128 };
      guint8 padded[24];
      memset (padded, 99, sizeof padded);
      memcpy (padded, packed, 8);
      memcpy (padded + 12, packed + 8, 8);
      SpiceDisplayPrimary primary
          = { .format = i ? SPICE_SURFACE_FMT_32_ARGB : SPICE_SURFACE_FMT_32_xRGB,
              .width = 2,
              .height = 2,
              .stride = 8,
              .data = packed };
      g_autoptr (GdkTexture) first = pirwa_frame_texture (&primary, NULL, NULL);
      primary.stride = 12;
      primary.data = padded;
      g_autoptr (GdkTexture) reference = pirwa_frame_texture (&primary, NULL, NULL);
      guint8 pixels[16], expected[16];
      gdk_texture_download (first, pixels, 8);
      gdk_texture_download (reference, expected, 8);
      g_assert_cmpmem (pixels, 16, expected, 16);
      memset (packed, 0, sizeof packed);
      memset (padded, 0, sizeof padded);
      gdk_texture_download (first, pixels, 8);
      g_assert_cmpmem (pixels, 16, expected, 16);
    }
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
  g_test_add_func ("/console/crop-and-ownership", test_cropped_frame);
  g_test_add_func ("/console/contiguous-rgb-and-alpha", test_contiguous_rgb);
  g_test_add_func ("/console/monitor-padding", test_monitor_padding);
  return g_test_run ();
}
