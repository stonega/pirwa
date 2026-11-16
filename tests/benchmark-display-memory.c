/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit Linux/glibc GUI diagnostics; never linked into Pirwa or meson test. */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-display.h"
#include "pirwa-frame.h"
#include <glib/gstdio.h>
#include <malloc.h>

typedef struct
{
  guint64 bytes;
  gboolean tile;
} Texture;

static GMutex texture_mutex;
static guint64 live_bytes, peak_bytes, full_textures, tile_textures;

static void
texture_released (gpointer data, GObject *object)
{
  (void)object;
  Texture *texture = data;
  g_mutex_lock (&texture_mutex);
  live_bytes -= texture->bytes;
  if (texture->tile)
    tile_textures--;
  else
    full_textures--;
  g_mutex_unlock (&texture_mutex);
  g_free (texture);
}

static GdkTexture *
track_texture (GdkTexture *object, guint64 bytes, gboolean tile)
{
  if (!object)
    return NULL;
  Texture *texture = g_new (Texture, 1);
  *texture = (Texture){ bytes, tile };
  g_mutex_lock (&texture_mutex);
  live_bytes += bytes;
  peak_bytes = MAX (peak_bytes, live_bytes);
  if (tile)
    tile_textures++;
  else
    full_textures++;
  g_mutex_unlock (&texture_mutex);
  g_object_weak_ref (G_OBJECT (object), texture_released, texture);
  return object;
}

GdkTexture *__real_pirwa_frame_texture (const SpiceDisplayPrimary *, GdkTexture *,
                                        cairo_region_t *);
GdkTexture *__real_pirwa_frame_texture_rect (const SpiceDisplayPrimary *,
                                             const cairo_rectangle_int_t *);

GdkTexture *
__wrap_pirwa_frame_texture (const SpiceDisplayPrimary *primary, GdkTexture *previous,
                            cairo_region_t *damage)
{
  return track_texture (__real_pirwa_frame_texture (primary, previous, damage),
                        (guint64)primary->width * primary->height * 4, FALSE);
}

GdkTexture *
__wrap_pirwa_frame_texture_rect (const SpiceDisplayPrimary *primary,
                                 const cairo_rectangle_int_t *rect)
{
  return track_texture (__real_pirwa_frame_texture_rect (primary, rect),
                        (guint64)rect->width * rect->height * 4, TRUE);
}

typedef struct
{
  GMainLoop *loop;
  GtkWindow *window;
  PirwaDisplay *display;
  SpiceDisplayPrimary primary;
  GString *samples;
  const char *phase;
  char *mode;
  char *heap_directory;
  char *renderer;
  int width, height, cycles, active_seconds, settle_seconds;
  int view_width, view_height, scale;
  gboolean windowed, mapped;
  guint cycle, stage, tick, sample_timer, timeout, sample_count, produced;
  gint64 started;
  int trim_result;
} Run;

static gint64
proc_value (const char *contents, const char *name)
{
  for (const char *line = contents; line && *line;)
    {
      if (g_str_has_prefix (line, name))
        return g_ascii_strtoll (line + strlen (name), NULL, 10);
      const char *next = strchr (line, '\n');
      line = next ? next + 1 : NULL;
    }
  return -1;
}

static void
dump_drm (const char *path)
{
  g_autoptr (GDir) directory = g_dir_open ("/proc/self/fdinfo", 0, NULL);
  g_autoptr (GString) drm = g_string_new (NULL);
  const char *name;
  while (directory && (name = g_dir_read_name (directory)))
    {
      g_autofree char *info = g_build_filename ("/proc/self/fdinfo", name, NULL);
      g_autofree char *contents = NULL;
      if (g_file_get_contents (info, &contents, NULL, NULL) && strstr (contents, "drm-driver:"))
        g_string_append_printf (drm, "fd=%s\n%s\n", name, contents);
    }
  if (!g_file_set_contents (path, drm->str, drm->len, NULL))
    g_error ("Cannot write GPU descriptor statistics");
}

static void
sample (Run *run, gboolean heap_dump)
{
  struct mallinfo2 mi = mallinfo2 ();
  g_autofree char *contents = NULL;
  if (!g_file_get_contents ("/proc/self/smaps_rollup", &contents, NULL, NULL))
    g_error ("Cannot read process memory statistics");
  guint64 bytes, peak, full, tiles;
  g_mutex_lock (&texture_mutex);
  bytes = live_bytes;
  peak = peak_bytes;
  full = full_textures;
  tiles = tile_textures;
  g_mutex_unlock (&texture_mutex);
  g_string_append_printf (
      run->samples,
      "%s{\"cycle\":%u,\"phase\":\"%s\",\"elapsed_us\":%" G_GINT64_FORMAT
      ",\"pss_kib\":%" G_GINT64_FORMAT ",\"anonymous_pss_kib\":%" G_GINT64_FORMAT
      ",\"file_pss_kib\":%" G_GINT64_FORMAT ",\"shared_pss_kib\":%" G_GINT64_FORMAT
      ",\"private_dirty_kib\":%" G_GINT64_FORMAT ",\"rss_kib\":%" G_GINT64_FORMAT
      ",\"live_texture_bytes\":%" G_GUINT64_FORMAT ",\"peak_texture_bytes\":%" G_GUINT64_FORMAT
      ",\"live_full_textures\":%" G_GUINT64_FORMAT ",\"live_tile_textures\":%" G_GUINT64_FORMAT
      ",\"malloc_arena_bytes\":%zu,\"malloc_in_use_bytes\":%zu,\"malloc_free_bytes\":%zu,"
      "\"malloc_mmap_bytes\":%zu,\"heap_dump\":%s}",
      run->sample_count ? "," : "", run->cycle, run->phase, g_get_monotonic_time () - run->started,
      proc_value (contents, "Pss:"), proc_value (contents, "Pss_Anon:"),
      proc_value (contents, "Pss_File:"), proc_value (contents, "Pss_Shmem:"),
      proc_value (contents, "Private_Dirty:"), proc_value (contents, "Rss:"), bytes, peak, full,
      tiles, mi.arena, mi.uordblks, mi.fordblks, mi.hblkhd,
      heap_dump && run->heap_directory ? "true" : "false");
  if (heap_dump && run->heap_directory)
    {
      g_autofree char *name
          = g_strdup_printf ("%03u-%u-%s.xml", run->sample_count, run->cycle, run->phase);
      g_autofree char *path = g_build_filename (run->heap_directory, name, NULL);
      FILE *file = g_fopen (path, "w");
      if (!file)
        g_error ("Cannot open allocator statistics file");
      int result = malloc_info (0, file);
      int closed = fclose (file);
      if (result != 0 || closed != 0)
        g_error ("Cannot write allocator statistics");
      g_autofree char *drm_name
          = g_strdup_printf ("%03u-%u-%s.drm", run->sample_count, run->cycle, run->phase);
      g_autofree char *drm_path = g_build_filename (run->heap_directory, drm_name, NULL);
      dump_drm (drm_path);
    }
  run->sample_count++;
}

static gboolean
sample_timer (gpointer data)
{
  sample (data, FALSE);
  return G_SOURCE_CONTINUE;
}

static void
phase (Run *run, const char *name)
{
  run->phase = name;
  sample (run, TRUE);
}

static gboolean
update (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  Run *run = data;
  int x = 0, y = 0, width = run->width, height = run->height;
  run->produced++;
  if (g_str_equal (run->mode, "invalidate-full"))
    {
      ((guint32 *)run->primary.data)[0]++;
      pirwa_display_invalidate (run->display, 0, 0, width, height);
      return G_SOURCE_CONTINUE;
    }
  if (g_str_equal (run->mode, "bands"))
    {
      y = (run->produced * 31) % (run->height - 31);
      height = 32;
    }
  else if (g_str_equal (run->mode, "small"))
    {
      x = run->width / 2 - 32;
      y = run->height / 2 - 32;
      width = height = 64;
    }
  guint32 *pixels = (guint32 *)run->primary.data;
  for (int row = y; row < y + height; row++)
    for (int col = x; col < x + width; col++)
      pixels[row * run->width + col] = (run->produced & 255) << 16 | (col & 255) << 8 | (row & 255);
  pirwa_display_invalidate (run->display, x, y, width, height);
  return G_SOURCE_CONTINUE;
}

static void create_window (Run *run);
static gboolean advance (gpointer data);

static gboolean
finish (gpointer data)
{
  Run *run = data;
  sample (run, TRUE);
  g_source_remove (run->sample_timer);
  run->sample_timer = 0;
  g_main_loop_quit (run->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
advance (gpointer data)
{
  Run *run = data;
  run->stage++;
  switch (run->stage)
    {
    case 1:
      run->scale = gdk_surface_get_scale_factor (gtk_native_get_surface (GTK_NATIVE (run->window)));
      run->view_width = gtk_widget_get_width (GTK_WIDGET (run->display));
      run->view_height = gtk_widget_get_height (GTK_WIDGET (run->display));
      run->tick = gtk_widget_add_tick_callback (GTK_WIDGET (run->display), update, run, NULL);
      phase (run, "active");
      g_timeout_add_seconds (run->active_seconds, advance, run);
      break;
    case 2:
      gtk_widget_remove_tick_callback (GTK_WIDGET (run->display), run->tick);
      run->tick = 0;
      phase (run, "idle");
      g_timeout_add_seconds (run->settle_seconds, advance, run);
      break;
    case 3:
      pirwa_display_clear (run->display);
      g_clear_pointer (&run->primary.data, g_free);
      phase (run, "cleared");
      g_timeout_add_seconds (run->settle_seconds, advance, run);
      break;
    case 4:
      gtk_window_destroy (run->window);
      run->window = NULL;
      run->display = NULL;
      phase (run, "closed");
      g_timeout_add_seconds (run->settle_seconds, advance, run);
      break;
    case 5:
      phase (run, "cycle-end");
      if (run->cycle < (guint)run->cycles)
        create_window (run);
      else
        {
          /* Diagnostic control after all windows/frames have been released.
           * This is never a production memory-management policy. */
          phase (run, "before-trim");
          run->trim_result = malloc_trim (0);
          phase (run, "trimmed");
          g_timeout_add_seconds (1, finish, run);
        }
      break;
    default:
      g_assert_not_reached ();
    }
  return G_SOURCE_REMOVE;
}

static void
mapped (GtkWidget *widget, Run *run)
{
  if (run->mapped)
    return;
  run->mapped = TRUE;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  const char *renderer = G_OBJECT_TYPE_NAME (gtk_native_get_renderer (GTK_NATIVE (widget)));
  if (!run->renderer)
    run->renderer = g_strdup (renderer);
  g_assert_cmpstr (run->renderer, ==, renderer);
  run->scale = gdk_surface_get_scale_factor (surface);
  run->view_width = gtk_widget_get_width (GTK_WIDGET (run->display));
  run->view_height = gtk_widget_get_height (GTK_WIDGET (run->display));
  phase (run, "warmup");
  g_timeout_add_seconds (2, advance, run);
}

static void
create_window (Run *run)
{
  run->cycle++;
  run->stage = 0;
  run->mapped = FALSE;
  run->primary = (SpiceDisplayPrimary){ .format = SPICE_SURFACE_FMT_32_xRGB,
                                        .width = run->width,
                                        .height = run->height,
                                        .stride = run->width * 4,
                                        .data = g_malloc0_n ((gsize)run->width * run->height, 4) };
  memset (run->primary.data, 42, (gsize)run->width * run->height * 4);
  run->window = GTK_WINDOW (gtk_window_new ());
  run->display = PIRWA_DISPLAY (pirwa_display_new ());
  gtk_window_set_child (run->window, GTK_WIDGET (run->display));
  gtk_window_set_default_size (run->window, 960, 640);
  gtk_window_set_title (run->window, "Pirwa Renderer Memory QA");
  g_assert_true (pirwa_display_set_primary (run->display, &run->primary));
  g_signal_connect (run->window, "map", G_CALLBACK (mapped), run);
  if (!run->windowed)
    gtk_window_fullscreen (run->window);
  gtk_window_present (run->window);
}

static gboolean
timed_out (gpointer data)
{
  (void)data;
  g_error ("Renderer memory diagnostic timed out");
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Run run = { .width = 3840,
              .height = 2160,
              .cycles = 3,
              .active_seconds = 3,
              .settle_seconds = 2,
              .phase = "baseline" };
  GOptionEntry options[]
      = { { "width", 0, 0, G_OPTION_ARG_INT, &run.width, "Source width (64–8192)", "PIXELS" },
          { "height", 0, 0, G_OPTION_ARG_INT, &run.height, "Source height (64–8192)", "PIXELS" },
          { "cycles", 0, 0, G_OPTION_ARG_INT, &run.cycles, "Open/draw/clear/close cycles (1–5)",
            "COUNT" },
          { "active-seconds", 0, 0, G_OPTION_ARG_INT, &run.active_seconds,
            "Drawing per cycle (2–15)", "SECONDS" },
          { "settle-seconds", 0, 0, G_OPTION_ARG_INT, &run.settle_seconds,
            "Idle/clear/close waits (1–10)", "SECONDS" },
          { "mode", 0, 0, G_OPTION_ARG_STRING, &run.mode, "full, invalidate-full, bands, or small",
            "MODE" },
          { "heap-directory", 0, 0, G_OPTION_ARG_FILENAME, &run.heap_directory,
            "New directory for allocator XML", "PATH" },
          { "windowed", 0, 0, G_OPTION_ARG_NONE, &run.windowed, "Use a 960-pixel window", NULL },
          { 0 } };
  g_autoptr (GOptionContext) context = g_option_context_new ("— renderer lifetime diagnostics");
  g_option_context_add_main_entries (context, options, NULL);
  g_autoptr (GError) error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error) || run.width < 64 || run.width > 8192
      || run.height < 64 || run.height > 8192 || run.cycles < 1 || run.cycles > 5
      || run.active_seconds < 2 || run.active_seconds > 15 || run.settle_seconds < 1
      || run.settle_seconds > 10
      || (run.mode && !g_str_equal (run.mode, "full") && !g_str_equal (run.mode, "invalidate-full")
          && !g_str_equal (run.mode, "bands") && !g_str_equal (run.mode, "small")))
    {
      g_printerr ("%s\n", error ? error->message : "Invalid memory diagnostic arguments");
      return 2;
    }
  if (!run.mode)
    run.mode = g_strdup ("bands");
  if (run.heap_directory && g_mkdir (run.heap_directory, 0700) != 0)
    g_error ("Heap directory must be new and writable");
  gtk_init ();
  run.loop = g_main_loop_new (NULL, FALSE);
  run.samples = g_string_new (NULL);
  run.started = g_get_monotonic_time ();
  sample (&run, TRUE);
  run.sample_timer = g_timeout_add_seconds (1, sample_timer, &run);
  run.timeout = g_timeout_add_seconds (300, timed_out, NULL);
  create_window (&run);
  g_main_loop_run (run.loop);
  g_print ("{\"width\":%d,\"height\":%d,\"cycles\":%d,\"active_seconds\":%d,"
           "\"settle_seconds\":%d,\"warmup_seconds\":2,\"mode\":\"%s\",\"renderer\":\"%s\","
           "\"scale\":%d,\"view_width\":%d,\"view_height\":%d,\"malloc_trim_result\":%d,"
           "\"samples\":[%s]}\n",
           run.width, run.height, run.cycles, run.active_seconds, run.settle_seconds, run.mode,
           run.renderer, run.scale, run.view_width, run.view_height, run.trim_result,
           run.samples->str);
  g_source_remove (run.timeout);
  g_string_free (run.samples, TRUE);
  g_main_loop_unref (run.loop);
  g_free (run.renderer);
  g_free (run.mode);
  g_free (run.heap_directory);
  return 0;
}
