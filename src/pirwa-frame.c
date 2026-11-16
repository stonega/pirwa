/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-frame.h"
#include <linux/input-event-codes.h>
#include <string.h>

gboolean
pirwa_frame_valid (const SpiceDisplayPrimary *p)
{
  if (!p->data || p->width < 1 || p->height < 1 || p->width > 8192 || p->height > 8192)
    return FALSE;
  gboolean rgb32 = p->format == SPICE_SURFACE_FMT_32_xRGB || p->format == SPICE_SURFACE_FMT_32_ARGB;
  if (!rgb32 && p->format != SPICE_SURFACE_FMT_16_555 && p->format != SPICE_SURFACE_FMT_16_565)
    return FALSE;
  return p->stride >= p->width * (rgb32 ? 4 : 2);
}

cairo_rectangle_int_t
pirwa_frame_monitor_area (const SpiceDisplayPrimary *primary,
                          const SpiceDisplayMonitorConfig *monitor)
{
  if (!pirwa_frame_valid (primary))
    return (cairo_rectangle_int_t){ 0 };
  cairo_rectangle_int_t area = { 0, 0, primary->width, primary->height };
  if (monitor && monitor->surface_id == 0 && monitor->width > 0 && monitor->height > 0
      && (guint64)monitor->x + monitor->width <= (guint)primary->width
      && (guint64)monitor->y + monitor->height <= (guint)primary->height)
    area = (cairo_rectangle_int_t){ monitor->x, monitor->y, monitor->width, monitor->height };
  return area;
}

GdkTexture *
pirwa_frame_texture_rect (const SpiceDisplayPrimary *p, const cairo_rectangle_int_t *rect)
{
  if (!pirwa_frame_valid (p) || rect->x < 0 || rect->y < 0 || rect->width < 1 || rect->height < 1
      || (gint64)rect->x + rect->width > p->width || (gint64)rect->y + rect->height > p->height)
    return NULL;
  int pixel_size
      = p->format == SPICE_SURFACE_FMT_32_xRGB || p->format == SPICE_SURFACE_FMT_32_ARGB ? 4 : 2;
  SpiceDisplayPrimary crop = *p;
  crop.width = rect->width;
  crop.height = rect->height;
  crop.data += (gsize)rect->y * p->stride + (gsize)rect->x * pixel_size;
  return pirwa_frame_texture (&crop, NULL, NULL);
}

GdkTexture *
pirwa_frame_texture (const SpiceDisplayPrimary *p, GdkTexture *previous, cairo_region_t *damage)
{
  if (!pirwa_frame_valid (p))
    return NULL;
  gboolean rgb32 = p->format == SPICE_SURFACE_FMT_32_xRGB || p->format == SPICE_SURFACE_FMT_32_ARGB;
  gsize stride = (gsize)p->width * 4;
  guint8 *copy = g_malloc_n (stride, p->height);
  /* Let libc use its large-copy path for the usual packed RGB surface. */
  if (rgb32 && (gsize)p->stride == stride)
    memcpy (copy, p->data, stride * p->height);
  else
    for (int y = 0; y < p->height; y++)
      {
        const guint8 *source = p->data + (gsize)y * p->stride;
        guint8 *row = copy + y * stride;
        if (rgb32)
          memcpy (row, source, stride);
        else
          for (int x = 0; x < p->width; x++)
            {
              guint16 pixel;
              memcpy (&pixel, source + x * 2, 2);
              guint red
                  = p->format == SPICE_SURFACE_FMT_16_565 ? (pixel >> 11) & 31 : (pixel >> 10) & 31;
              guint green = (pixel >> 5) & (p->format == SPICE_SURFACE_FMT_16_565 ? 63 : 31);
              row[x * 4] = (pixel & 31) * 255 / 31;
              row[x * 4 + 1] = green * 255 / (p->format == SPICE_SURFACE_FMT_16_565 ? 63 : 31);
              row[x * 4 + 2] = red * 255 / 31;
              row[x * 4 + 3] = 255;
            }
      }
  /* SPICE owns and mutates primary memory. A texture must keep its own immutable
   * snapshot; the previous texture may still be in use by the render thread. */
  g_autoptr (GBytes) bytes = g_bytes_new_take (copy, stride * p->height);
  g_autoptr (GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new ();
  gdk_memory_texture_builder_set_width (builder, p->width);
  gdk_memory_texture_builder_set_height (builder, p->height);
  gdk_memory_texture_builder_set_stride (builder, stride);
  gdk_memory_texture_builder_set_bytes (builder, bytes);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  gdk_memory_texture_builder_set_format (
      builder, p->format == SPICE_SURFACE_FMT_32_ARGB ? GDK_MEMORY_B8G8R8A8 : GDK_MEMORY_B8G8R8X8);
#else
  gdk_memory_texture_builder_set_format (
      builder,
      rgb32 ? (p->format == SPICE_SURFACE_FMT_32_ARGB ? GDK_MEMORY_A8R8G8B8 : GDK_MEMORY_X8R8G8B8)
            : GDK_MEMORY_B8G8R8X8);
#endif
  if (previous && damage && gdk_texture_get_width (previous) == p->width
      && gdk_texture_get_height (previous) == p->height)
    {
      gdk_memory_texture_builder_set_update_texture (builder, previous);
      gdk_memory_texture_builder_set_update_region (builder, damage);
    }
  return gdk_memory_texture_builder_build (builder);
}

gboolean
pirwa_frame_position (int width, int height, int guest_width, int guest_height, double x, double y,
                      int *guest_x, int *guest_y)
{
  if (width < 1 || height < 1 || guest_width < 1 || guest_height < 1)
    return FALSE;
  double scale = MIN ((double)width / guest_width, (double)height / guest_height);
  double left = (width - guest_width * scale) / 2;
  double top = (height - guest_height * scale) / 2;
  if (x < left || y < top || x >= width - left || y >= height - top)
    return FALSE;
  *guest_x = CLAMP ((int)((x - left) / scale), 0, guest_width - 1);
  *guest_y = CLAMP ((int)((y - top) / scale), 0, guest_height - 1);
  return TRUE;
}

guint
pirwa_frame_scancode (guint keycode)
{
  /* GNOME's Linux Wayland and Xorg evdev keycodes include the XKB offset of 8.
   * The guest protocol uses XT set 1; extended keys need the E0 flag (0x100). */
  if (keycode < 8)
    return 0;
  guint key = keycode - 8;
  if ((key >= KEY_ESC && key <= KEY_KPDOT) || key == KEY_F11 || key == KEY_F12 || key == KEY_102ND)
    return key;
  switch (key)
    {
    case KEY_KPENTER:
      return 0x11c;
    case KEY_RIGHTCTRL:
      return 0x11d;
    case KEY_KPSLASH:
      return 0x135;
    case KEY_SYSRQ:
      return 0x137;
    case KEY_RIGHTALT:
      return 0x138;
    case KEY_HOME:
      return 0x147;
    case KEY_UP:
      return 0x148;
    case KEY_PAGEUP:
      return 0x149;
    case KEY_LEFT:
      return 0x14b;
    case KEY_RIGHT:
      return 0x14d;
    case KEY_END:
      return 0x14f;
    case KEY_DOWN:
      return 0x150;
    case KEY_PAGEDOWN:
      return 0x151;
    case KEY_INSERT:
      return 0x152;
    case KEY_DELETE:
      return 0x153;
    case KEY_LEFTMETA:
      return 0x15b;
    case KEY_RIGHTMETA:
      return 0x15c;
    case KEY_COMPOSE:
      return 0x15d;
    default:
      return 0;
    }
}
