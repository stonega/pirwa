/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-display.h"
#include "config.h"
#include "pirwa-frame.h"
#include <glib/gi18n.h>

/* Keep one immutable full frame and a bounded set of replacement tiles. Large
 * updates compact to one full frame. Small updates reuse the base GPU texture.
 * The limit bounds texture count, retained memory and overdraw. */
#define TILE_SIZE 256
#define MAX_PATCHES 32

typedef struct
{
  cairo_rectangle_int_t rect;
  cairo_rectangle_int_t sample;
  GdkTexture *texture;
} Patch;

struct _PirwaDisplay
{
  GtkWidget parent_instance;
  SpiceDisplayPrimary primary;
  GdkTexture *texture;
  cairo_region_t *damage;
  Patch patches[MAX_PATCHES];
  guint n_patches;
  int allocated_width;
  int allocated_height;
  gboolean rounded;
};
G_DEFINE_FINAL_TYPE (PirwaDisplay, pirwa_display, GTK_TYPE_WIDGET)

void
pirwa_display_set_rounded (PirwaDisplay *self, gboolean rounded)
{
  if (self->rounded == rounded)
    return;
  self->rounded = rounded;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
clear_patches (PirwaDisplay *self)
{
  for (guint i = 0; i < self->n_patches; i++)
    g_clear_object (&self->patches[i].texture);
  self->n_patches = 0;
}

void
pirwa_display_clear (PirwaDisplay *self)
{
  clear_patches (self);
  memset (&self->primary, 0, sizeof self->primary);
  g_clear_object (&self->texture);
  g_clear_pointer (&self->damage, cairo_region_destroy);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pirwa_display_invalidate (PirwaDisplay *self, int x, int y, int width, int height)
{
  if (!self->primary.data || width <= 0 || height <= 0)
    return;
  cairo_rectangle_int_t rect = { MAX (0, x), MAX (0, y), 0, 0 };
  rect.width = MIN ((gint64)self->primary.width, (gint64)x + width) - rect.x;
  rect.height = MIN ((gint64)self->primary.height, (gint64)y + height) - rect.y;
  if (rect.width <= 0 || rect.height <= 0)
    return;
  if (!self->damage)
    self->damage = cairo_region_create ();
  cairo_region_union_rectangle (self->damage, &rect);
  if (cairo_region_num_rectangles (self->damage) > 64)
    {
      cairo_region_get_extents (self->damage, &rect);
      cairo_region_destroy (self->damage);
      self->damage = cairo_region_create_rectangle (&rect);
    }
  /* Queue only when data changes; GTK coalesces updates before a snapshot. */
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
pirwa_display_set_primary (PirwaDisplay *self, const SpiceDisplayPrimary *primary)
{
  pirwa_display_clear (self);
  if (!pirwa_frame_valid (primary))
    return FALSE;
  self->primary = *primary;
  pirwa_display_invalidate (self, 0, 0, primary->width, primary->height);
  return TRUE;
}

static void
size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  (void)baseline;
  PirwaDisplay *self = PIRWA_DISPLAY (widget);
  if (width != self->allocated_width || height != self->allocated_height)
    {
      self->allocated_width = width;
      self->allocated_height = height;
      pirwa_display_invalidate (self, 0, 0, self->primary.width, self->primary.height);
      g_signal_emit_by_name (self, "resize", width, height);
    }
}

static gboolean
update_patches (PirwaDisplay *self)
{
  /* Overlays must be opaque to replace the base. Preserve ARGB alpha semantics
   * using the full-frame path, rather than blending a patch over old pixels. */
  if (!self->texture || self->primary.format == SPICE_SURFACE_FMT_32_ARGB)
    return FALSE;
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  /* Cairo rounds independently transformed cropped surfaces differently.
   * Keep the exact full-frame path for the software renderer. */
  if (native && GSK_IS_CAIRO_RENDERER (gtk_native_get_renderer (native)))
    return FALSE;
  double scale = MIN ((double)gtk_widget_get_width (GTK_WIDGET (self)) / self->primary.width,
                      (double)gtk_widget_get_height (GTK_WIDGET (self)) / self->primary.height)
                 * gtk_widget_get_scale_factor (GTK_WIDGET (self));
  if (scale <= 0)
    return FALSE;
  double footprint = MAX (2.0, 2.0 / scale);
  if (footprint > 32)
    return FALSE; /* Bound sampling padding for extreme downscales. */
  int halo = 2;
  while (halo < footprint)
    halo *= 2; /* Align cropped mipmap grids with the full image. */
  /* Odd mipmap dimensions change the sampling phase of a full image relative
   * to its cropped tiles. Retain the exact full-image path when minifying a
   * source that does not align with the sampling grid. */
  if (scale < 1.0 && (self->primary.width % halo || self->primary.height % halo))
    return FALSE;
  cairo_rectangle_int_t bounds;
  cairo_region_get_extents (self->damage, &bounds);
  int width = self->primary.width, height = self->primary.height;
  int columns = (width + TILE_SIZE - 1) / TILE_SIZE;
  int rows = (height + TILE_SIZE - 1) / TILE_SIZE;
  guint n_tiles = columns * rows;
  /* Stack space is bounded by the supported 8192 × 8192 framebuffer. */
  gboolean dirty[1024] = { FALSE };
  guint additions = 0;
  gint64 area = 0;
  for (guint i = 0; i < self->n_patches; i++)
    area += (gint64)self->patches[i].rect.width * self->patches[i].rect.height;
  for (int y = MAX (0, bounds.y - halo) / TILE_SIZE;
       y <= MIN (height - 1, bounds.y + bounds.height + halo - 1) / TILE_SIZE; y++)
    for (int x = MAX (0, bounds.x - halo) / TILE_SIZE;
         x <= MIN (width - 1, bounds.x + bounds.width + halo - 1) / TILE_SIZE; x++)
      {
        cairo_rectangle_int_t rect
            = { x * TILE_SIZE, y * TILE_SIZE, MIN (TILE_SIZE, width - x * TILE_SIZE),
                MIN (TILE_SIZE, height - y * TILE_SIZE) };
        /* A changed boundary pixel also invalidates the adjacent tile's halo. */
        cairo_rectangle_int_t sample = { MAX (0, rect.x - halo), MAX (0, rect.y - halo), 0, 0 };
        sample.width = MIN (width, rect.x + rect.width + halo) - sample.x;
        sample.height = MIN (height, rect.y + rect.height + halo) - sample.y;
        if (cairo_region_contains_rectangle (self->damage, &sample) == CAIRO_REGION_OVERLAP_OUT)
          continue;
        dirty[y * columns + x] = TRUE;
        gboolean known = FALSE;
        for (guint i = 0; i < self->n_patches; i++)
          known |= self->patches[i].rect.x == rect.x && self->patches[i].rect.y == rect.y;
        if (!known)
          {
            additions++;
            area += (gint64)rect.width * rect.height;
          }
      }
  if (self->n_patches + additions > MAX_PATCHES || area * 4 > (gint64)width * height)
    return FALSE;
  for (guint tile = 0; tile < n_tiles; tile++)
    if (dirty[tile])
      {
        int x = (tile % columns) * TILE_SIZE, y = (tile / columns) * TILE_SIZE;
        guint i;
        for (i = 0; i < self->n_patches; i++)
          if (self->patches[i].rect.x == x && self->patches[i].rect.y == y)
            break;
        if (i == self->n_patches)
          self->n_patches++;
        Patch *patch = &self->patches[i];
        patch->rect = (cairo_rectangle_int_t){ x, y, MIN (TILE_SIZE, width - x),
                                               MIN (TILE_SIZE, height - y) };
        /* Include the bilinear footprint after snapping clips to device pixels. */
        patch->sample = (cairo_rectangle_int_t){ MAX (0, x - halo), MAX (0, y - halo), 0, 0 };
        patch->sample.width = MIN (width, x + patch->rect.width + halo) - patch->sample.x;
        patch->sample.height = MIN (height, y + patch->rect.height + halo) - patch->sample.y;
        GdkTexture *texture = pirwa_frame_texture_rect (&self->primary, &patch->sample);
        g_assert_nonnull (texture); /* Validated primary and clipped tile. */
        g_set_object (&patch->texture, texture);
        g_object_unref (texture);
      }
  return TRUE;
}

static void
append_source_texture (GtkSnapshot *snapshot, GdkTexture *texture, float x, float y, float scale,
                       const graphene_rect_t *source)
{
  /* Keep all sampling in a common source-pixel coordinate system. Scaling
   * cropped bounds separately changes the sampling phase on HiDPI surfaces. */
  graphene_point_t origin = GRAPHENE_POINT_INIT (x, y);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &origin);
  gtk_snapshot_scale (snapshot, scale, scale);
  gtk_snapshot_append_texture (snapshot, texture, source);
  gtk_snapshot_restore (snapshot);
}

static void
snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  PirwaDisplay *self = PIRWA_DISPLAY (widget);
  gtk_snapshot_save (snapshot);
  graphene_rect_t bounds
      = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  const GdkRGBA black = { 0, 0, 0, 1 };
  if (self->primary.data && self->damage && !cairo_region_is_empty (self->damage))
    {
      if (!update_patches (self))
        {
          /* A compacted base differs in every prior patch, not just today's
           * damage. Omit the old-base hint when replacing any cached patches. */
          GdkTexture *frame
              = pirwa_frame_texture (&self->primary, self->n_patches ? NULL : self->texture,
                                     self->n_patches ? NULL : self->damage);
          g_set_object (&self->texture, frame);
          g_clear_object (&frame);
          clear_patches (self);
        }
      g_clear_pointer (&self->damage, cairo_region_destroy);
    }
  if (!self->texture)
    {
      gtk_snapshot_restore (snapshot);
      return;
    }
  int width = gdk_texture_get_width (self->texture);
  int height = gdk_texture_get_height (self->texture);
  double scale = MIN (bounds.size.width / width, bounds.size.height / height);
  graphene_rect_t rect = GRAPHENE_RECT_INIT ((bounds.size.width - width * scale) / 2,
                                             (bounds.size.height - height * scale) / 2,
                                             width * scale, height * scale);
  /* A single opaque texture filling the widget can be rounded directly. It
   * has no overlapping layers or fractional letterbox edges to composite. */
  if (self->n_patches == 0 && self->primary.format != SPICE_SURFACE_FMT_32_ARGB
      && bounds.size.width > 0 && bounds.size.height > 0
      && (gint64)width * gtk_widget_get_height (widget)
             == (gint64)height * gtk_widget_get_width (widget))
    {
      if (self->rounded)
        {
          GskRoundedRect desktop;
          gsk_rounded_rect_init_from_rect (&desktop, &rect, 12);
          gtk_snapshot_push_rounded_clip (snapshot, &desktop);
        }
      gtk_snapshot_append_texture (snapshot, self->texture, &rect);
      if (self->rounded)
        gtk_snapshot_pop (snapshot);
      gtk_snapshot_restore (snapshot);
      return;
    }
  /* A luminance mask composites the desktop before rounding its edges. An
   * alpha clip can blend each replacement tile over the old corners again. */
  GskRoundedRect desktop;
  const GdkRGBA white = { 1, 1, 1, 1 };
  gsk_rounded_rect_init_from_rect (&desktop, &rect, self->rounded ? 12 : 0);
  gtk_snapshot_push_mask (snapshot, GSK_MASK_MODE_LUMINANCE);
  gtk_snapshot_push_rounded_clip (snapshot, &desktop);
  gtk_snapshot_append_color (snapshot, &white, &rect);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_append_color (snapshot, &black, &bounds);
  graphene_rect_t source = GRAPHENE_RECT_INIT (0, 0, width, height);
  if (self->n_patches == 0)
    gtk_snapshot_append_texture (snapshot, self->texture, &rect);
  else
    append_source_texture (snapshot, self->texture, rect.origin.x, rect.origin.y, scale, &source);
  for (guint i = 0; i < self->n_patches; i++)
    {
      Patch *patch = &self->patches[i];
      graphene_rect_t clip = GRAPHENE_RECT_INIT (
          rect.origin.x + patch->rect.x * scale, rect.origin.y + patch->rect.y * scale,
          patch->rect.width * scale, patch->rect.height * scale);
      int device_scale = gtk_widget_get_scale_factor (widget);
      float right
          = (int)((clip.origin.x + clip.size.width) * device_scale + 0.5) / (float)device_scale;
      float bottom
          = (int)((clip.origin.y + clip.size.height) * device_scale + 0.5) / (float)device_scale;
      clip.origin.x = (int)(clip.origin.x * device_scale + 0.5) / (float)device_scale;
      clip.origin.y = (int)(clip.origin.y * device_scale + 0.5) / (float)device_scale;
      clip.size.width = right - clip.origin.x;
      clip.size.height = bottom - clip.origin.y;
      /* Avoid applying the texture's outer-edge coverage a second time through
       * the clip. Only internal tile edges need clipping. */
      if (patch->rect.x == 0)
        {
          clip.size.width += clip.origin.x;
          clip.origin.x = 0;
        }
      if (patch->rect.y == 0)
        {
          clip.size.height += clip.origin.y;
          clip.origin.y = 0;
        }
      if (patch->rect.x + patch->rect.width == self->primary.width)
        clip.size.width = bounds.size.width - clip.origin.x;
      if (patch->rect.y + patch->rect.height == self->primary.height)
        clip.size.height = bounds.size.height - clip.origin.y;
      graphene_rect_t sample = GRAPHENE_RECT_INIT (patch->sample.x, patch->sample.y,
                                                   patch->sample.width, patch->sample.height);
      gtk_snapshot_push_clip (snapshot, &clip);
      /* Replace partial-coverage edge pixels, rather than blending them over
       * old image content. The device-pixel clip has no fractional edge. */
      gtk_snapshot_append_color (snapshot, &black, &clip);
      append_source_texture (snapshot, patch->texture, rect.origin.x, rect.origin.y, scale,
                             &sample);
      gtk_snapshot_pop (snapshot);
    }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
dispose (GObject *object)
{
  PirwaDisplay *self = PIRWA_DISPLAY (object);
  clear_patches (self);
  g_clear_object (&self->texture);
  g_clear_pointer (&self->damage, cairo_region_destroy);
  G_OBJECT_CLASS (pirwa_display_parent_class)->dispose (object);
}

static void
pirwa_display_class_init (PirwaDisplayClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = snapshot;
  GTK_WIDGET_CLASS (klass)->size_allocate = size_allocate;
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_IMG);
  g_signal_new ("resize", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void
scale_changed (PirwaDisplay *self, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  pirwa_display_invalidate (self, 0, 0, self->primary.width, self->primary.height);
}

static void
pirwa_display_init (PirwaDisplay *self)
{
  self->rounded = TRUE;
  g_signal_connect (self, "notify::scale-factor", G_CALLBACK (scale_changed), NULL);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Virtual machine display"), -1);
}

GtkWidget *
pirwa_display_new (void)
{
  return g_object_new (PIRWA_TYPE_DISPLAY, NULL);
}
