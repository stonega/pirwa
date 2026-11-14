/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit GUI QA; never registered in the default, display-free test suite. */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-display.h"
#include <time.h>

typedef struct
{
  gint64 paint_wall_us;
  gint64 paint_cpu_us;
  gint64 producer_cpu_us;
  gint64 interval_us;
} Sample;

typedef struct
{
  GtkWindow *window;
  PirwaDisplay *display;
  GdkFrameClock *clock;
  GMainLoop *loop;
  SpiceDisplayPrimary primary;
  GArray *samples;
  const char *mode;
  const char *png;
  guint tick;
  guint timeout;
  guint warmup;
  guint idle_paints;
  guint verification_step;
  guint max_channel_error;
  gboolean idle;
  int idle_seconds;
  gint64 idle_start;
  gint64 idle_cpu_start;
  gint64 idle_wall_us;
  gint64 idle_cpu_us;
  guint produced;
  guint painted;
  gint64 paint_start;
  gint64 paint_cpu_start;
  gint64 producer_cpu;
  gint64 last_paint;
  gint64 sample_start;
  gint64 sample_cpu_start;
  gint64 sample_end;
  gint64 sample_cpu_end;
  gint64 pss_first;
  gint64 pss_last;
  int frames;
  int expect_scale;
  gboolean force_full_compare;
  int width;
  int height;
} Run;

static gint64
cpu_time (clockid_t clock)
{
  struct timespec time;
  g_assert_cmpint (clock_gettime (clock, &time), ==, 0);
  return (gint64)time.tv_sec * G_USEC_PER_SEC + time.tv_nsec / 1000;
}

static gint64
pss (void)
{
  g_autofree char *contents = NULL;
  if (!g_file_get_contents ("/proc/self/smaps_rollup", &contents, NULL, NULL))
    return -1;
  const char *start = strstr (contents, "\nPss:");
  return start ? g_ascii_strtoll (start + 5, NULL, 10) : -1;
}

static guint32
color (int x, int y, guint frame)
{
  return ((frame & 255) << 16) | (((guint)x / 32 & 255) << 8) | ((guint)y / 32 & 255);
}

static void
write_rect (Run *run, int x, int y, int width, int height)
{
  guint32 *pixels = (guint32 *)run->primary.data;
  for (int row = y; row < y + height; row++)
    for (int col = x; col < x + width; col++)
      pixels[row * run->width + col] = color (col, row, run->produced);
  pirwa_display_invalidate (run->display, x, y, width, height);
}

static GdkTexture *
render_display (Run *run, PirwaDisplay *display)
{
  /* Invoke our widget's snapshot vfunc for a synchronous correctness capture,
   * outside timing spans. The normal benchmark uses GTK's regular frame clock. */
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GtkWidget *widget = GTK_WIDGET (display);
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (run->display));
  gtk_snapshot_scale (snapshot, scale, scale);
  GTK_WIDGET_GET_CLASS (widget)->snapshot (widget, snapshot);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget) * scale,
                                               gtk_widget_get_height (widget) * scale);
  return gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (run->window)), node,
                                      &bounds);
}

static GdkTexture *
render (Run *run)
{
  return render_display (run, run->display);
}

static void
compare_full_frame (Run *run, PirwaDisplay *reference)
{
  run->verification_step++;
  if (run->force_full_compare)
    pirwa_display_invalidate (run->display, 0, 0, run->width, run->height);
  pirwa_display_invalidate (reference, 0, 0, run->width, run->height);
  g_autoptr (GdkTexture) tiled = render (run);
  g_autoptr (GdkTexture) full = render_display (run, reference);
  int width = gdk_texture_get_width (tiled), height = gdk_texture_get_height (tiled);
  gsize size = (gsize)width * height * 4;
  g_autofree guint8 *a = g_malloc (size);
  g_autofree guint8 *b = g_malloc (size);
  gdk_texture_download (tiled, a, width * 4);
  gdk_texture_download (full, b, width * 4);
  guint difference = 0;
  gsize at = 0;
  for (gsize i = 0; i < size; i++)
    if (ABS ((int)a[i] - b[i]) > (int)difference)
      {
        difference = ABS ((int)a[i] - b[i]);
        at = i;
      }
  run->max_channel_error = MAX (run->max_channel_error, difference);
  if (difference > 1)
    g_error ("Sparse/full mismatch at step %u pixel %zu,%zu channel %zu: %u vs %u (max %u)",
             run->verification_step, at / 4 % width, at / 4 / width, at % 4, a[at], b[at],
             difference);
}

static void
verify_updates (Run *run)
{
  g_autoptr (PirwaDisplay) reference = g_object_ref_sink (PIRWA_DISPLAY (pirwa_display_new ()));
  gtk_widget_allocate (GTK_WIDGET (reference), gtk_widget_get_width (GTK_WIDGET (run->display)),
                       gtk_widget_get_height (GTK_WIDGET (run->display)), -1, NULL);
  g_assert_true (pirwa_display_set_primary (reference, &run->primary));
  compare_full_frame (run, reference);
  for (int step = 0; step < 36; step++)
    {
      run->produced += 31;
      int x = step < 6 ? MIN (run->width - 1, 254 + step) : (step * 257) % run->width;
      int y = step < 6 ? MIN (run->height - 1, 254 + step) : (step * 263) % run->height;
      write_rect (run, x, y, MIN (17, run->width - x), MIN (19, run->height - y));
      /* Out-of-bounds and overflowing invalidations must remain harmless. */
      pirwa_display_invalidate (run->display, -5, -5, 5, 5);
      pirwa_display_invalidate (run->display, G_MAXINT, G_MAXINT, G_MAXINT, G_MAXINT);
      if (step == 18)
        for (int i = 0; i < 80; i++)
          write_rect (run, (i * 31) % run->width, (i * 47) % run->height, 1, 1);
      compare_full_frame (run, reference);
    }
  /* Translucent ARGB must use replacement semantics, not blend over old tiles. */
  run->primary.format = SPICE_SURFACE_FMT_32_ARGB;
  guint32 *pixels = (guint32 *)run->primary.data;
  for (int i = 0; i < run->width * run->height; i++)
    pixels[i] = 0x80302010;
  g_assert_true (pirwa_display_set_primary (run->display, &run->primary));
  g_assert_true (pirwa_display_set_primary (reference, &run->primary));
  compare_full_frame (run, reference);
  for (int i = 0; i < 3; i++)
    {
      int x = run->width / 2 + i;
      pixels[(run->height / 2) * run->width + x] = 0x400080ff;
      pirwa_display_invalidate (run->display, x, run->height / 2, 1, 1);
      compare_full_frame (run, reference);
    }
  run->primary.format = SPICE_SURFACE_FMT_32_xRGB;
  g_assert_true (pirwa_display_set_primary (run->display, &run->primary));
  pirwa_display_clear (reference);
}

static void
verify_center (Run *run, GdkTexture *texture, guint32 expected)
{
  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);
  g_autofree guint8 *pixels = g_malloc_n ((gsize)width * height, 4);
  gdk_texture_download (texture, pixels, (gsize)width * 4);
  const guint8 *center = pixels + ((gsize)(height / 2) * width + width / 2) * 4;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  const guint8 wanted[] = { expected & 255, (expected >> 8) & 255, (expected >> 16) & 255, 255 };
#else
  const guint8 wanted[] = { 255, (expected >> 16) & 255, (expected >> 8) & 255, expected & 255 };
#endif
  (void)run;
  for (guint i = 0; i < 4; i++)
    g_assert_cmpint (ABS ((int)center[i] - wanted[i]), <=, 1);
}

static gboolean
finish (gpointer data)
{
  Run *run = data;
  if (run->idle)
    {
      run->idle_wall_us = g_get_monotonic_time () - run->idle_start;
      run->idle_cpu_us = cpu_time (CLOCK_PROCESS_CPUTIME_ID) - run->idle_cpu_start;
    }
  run->pss_last = pss ();
  verify_updates (run);
  /* Uniform center patches avoid interpolation/rounding ambiguity in captures. */
  run->produced = 173;
  write_rect (run, run->width / 2 - 16, run->height / 2 - 16, 32, 32);
  g_autoptr (GdkTexture) texture = render (run);
  verify_center (run, texture, color (run->width / 2, run->height / 2, run->produced));
  if (run->png)
    g_assert_true (gdk_texture_save_to_png (texture, run->png));
  pirwa_display_clear (run->display);
  g_clear_pointer (&run->primary.data, g_free);
  g_autoptr (GdkTexture) empty = render (run);
  verify_center (run, empty, 0);
  /* Already-rendered textures remain valid after transport memory is freed. */
  verify_center (run, texture, color (run->width / 2, run->height / 2, run->produced));
  guint8 invalid = 0;
  SpiceDisplayPrimary bad = { .width = 8193, .height = 1, .stride = 1, .data = &invalid };
  g_assert_false (pirwa_display_set_primary (run->display, &bad));
  /* Recreate a primary after teardown, including a different format and size. */
  guint16 rgb565[] = { 0x07e0 };
  SpiceDisplayPrimary replacement = { .width = 1,
                                      .height = 1,
                                      .stride = 2,
                                      .format = SPICE_SURFACE_FMT_16_565,
                                      .data = (guint8 *)rgb565 };
  g_assert_true (pirwa_display_set_primary (run->display, &replacement));
  g_autoptr (GdkTexture) recreated = render (run);
  verify_center (run, recreated, 0x00ff00);
  pirwa_display_clear (run->display);
  g_main_loop_quit (run->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
begin_idle (gpointer data)
{
  Run *run = data;
  run->idle = TRUE;
  run->idle_start = g_get_monotonic_time ();
  run->idle_cpu_start = cpu_time (CLOCK_PROCESS_CPUTIME_ID);
  g_timeout_add (run->idle_seconds * 1000, finish, run);
  return G_SOURCE_REMOVE;
}

static void
before_paint (GdkFrameClock *clock, Run *run)
{
  (void)clock;
  run->paint_start = g_get_monotonic_time ();
  run->paint_cpu_start = cpu_time (CLOCK_THREAD_CPUTIME_ID);
}

static void
after_paint (GdkFrameClock *clock, Run *run)
{
  (void)clock;
  if (run->idle)
    run->idle_paints++;
  if (run->painted == run->produced)
    return;
  run->painted = run->produced;
  gint64 now = g_get_monotonic_time ();
  if (run->produced > run->warmup)
    {
      Sample sample = { .paint_wall_us = now - run->paint_start,
                        .paint_cpu_us = cpu_time (CLOCK_THREAD_CPUTIME_ID) - run->paint_cpu_start,
                        .producer_cpu_us = run->producer_cpu,
                        .interval_us = now - run->last_paint };
      g_array_append_val (run->samples, sample);
      if (run->samples->len == (guint)run->frames)
        {
          run->sample_end = now;
          run->sample_cpu_end = cpu_time (CLOCK_PROCESS_CPUTIME_ID);
          gtk_widget_remove_tick_callback (GTK_WIDGET (run->display), run->tick);
          run->tick = 0;
          if (run->idle_seconds)
            g_timeout_add (200, begin_idle, run);
          else
            g_idle_add (finish, run);
        }
    }
  else if (run->produced == run->warmup)
    {
      if (run->expect_scale)
        {
          g_assert_cmpint (gtk_widget_get_scale_factor (GTK_WIDGET (run->display)), ==,
                           run->expect_scale);
          g_assert_cmpint (
              gdk_surface_get_scale_factor (gtk_native_get_surface (GTK_NATIVE (run->window))), ==,
              run->expect_scale);
        }
      run->sample_start = now;
      run->sample_cpu_start = cpu_time (CLOCK_PROCESS_CPUTIME_ID);
      run->pss_first = pss ();
    }
  run->last_paint = now;
}

static gboolean
update (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  Run *run = data;
  run->produced++;
  gint64 start = cpu_time (CLOCK_THREAD_CPUTIME_ID);
  if (g_str_equal (run->mode, "full"))
    write_rect (run, 0, 0, run->width, run->height);
  else
    {
      int x = run->width / 2 - 32;
      int y = run->height / 2 - 32;
      write_rect (run, x, y, 64, 64);
      if (g_str_equal (run->mode, "burst"))
        for (int i = 0; i < 15; i++)
          write_rect (run, x + i, y + i, 8, 8);
    }
  run->producer_cpu = cpu_time (CLOCK_THREAD_CPUTIME_ID) - start;
  return G_SOURCE_CONTINUE;
}

static void
mapped (GtkWidget *widget, Run *run)
{
  run->clock = g_object_ref (gtk_widget_get_frame_clock (widget));
  g_signal_connect (run->clock, "before-paint", G_CALLBACK (before_paint), run);
  g_signal_connect (run->clock, "after-paint", G_CALLBACK (after_paint), run);
  run->tick = gtk_widget_add_tick_callback (GTK_WIDGET (run->display), update, run, NULL);
}

static gboolean
timed_out (gpointer data)
{
  (void)data;
  g_error ("Display benchmark watchdog expired");
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Run run = { .width = 1920, .height = 1080, .frames = 180, .mode = "small", .warmup = 30 };
  g_autofree char *mode_arg = NULL;
  g_autofree char *png_arg = NULL;
  GOptionEntry options[]
      = { { "width", 0, 0, G_OPTION_ARG_INT, &run.width, "Source framebuffer width (64–8192)",
            "PIXELS" },
          { "force-full-compare", 0, 0, G_OPTION_ARG_NONE, &run.force_full_compare,
            "Sanity control: compare two full-frame renderings", NULL },
          { "expect-scale", 0, 0, G_OPTION_ARG_INT, &run.expect_scale,
            "Fail unless the mapped surface has this actual scale (1–4)", "FACTOR" },
          { "height", 0, 0, G_OPTION_ARG_INT, &run.height, "Source framebuffer height (64–8192)",
            "PIXELS" },
          { "idle-seconds", 0, 0, G_OPTION_ARG_INT, &run.idle_seconds,
            "Passive idle sample after rendering (0–60)", "SECONDS" },
          { "frames", 0, 0, G_OPTION_ARG_INT, &run.frames, "Measured frames (30–1800)", "COUNT" },
          { "mode", 0, 0, G_OPTION_ARG_STRING, &mode_arg, "Damage pattern: full, small, burst",
            "PATTERN" },
          { "snapshot", 0, 0, G_OPTION_ARG_STRING, &png_arg,
            "Save a correctness snapshot after timing", "PNG" },
          { 0 } };
  g_autoptr (GOptionContext) context = g_option_context_new ("— production display renderer QA");
  g_option_context_add_main_entries (context, options, NULL);
  g_autoptr (GError) error = NULL;
  gboolean parsed = g_option_context_parse (context, &argc, &argv, &error);
  if (mode_arg)
    run.mode = mode_arg;
  run.png = png_arg;
  if (!parsed || run.expect_scale < 0 || run.expect_scale > 4 || run.idle_seconds < 0
      || run.idle_seconds > 60 || run.width < 64 || run.width > 8192 || run.height < 64
      || run.height > 8192 || run.frames < 30 || run.frames > 1800
      || (!g_str_equal (run.mode, "full") && !g_str_equal (run.mode, "small")
          && !g_str_equal (run.mode, "burst")))
    {
      g_printerr ("%s\n", error ? error->message : "Invalid display benchmark arguments");
      return 2;
    }
  gtk_init ();
  run.loop = g_main_loop_new (NULL, FALSE);
  run.samples = g_array_new (FALSE, FALSE, sizeof (Sample));
  run.primary = (SpiceDisplayPrimary){ .format = SPICE_SURFACE_FMT_32_xRGB,
                                       .width = run.width,
                                       .height = run.height,
                                       .stride = run.width * 4,
                                       .data = g_malloc0_n ((gsize)run.width * run.height, 4) };
  run.window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_title (run.window, "Pirwa Console Renderer QA");
  gtk_window_set_default_size (run.window, 960, 640);
  run.display = PIRWA_DISPLAY (pirwa_display_new ());
  gtk_window_set_child (run.window, GTK_WIDGET (run.display));
  g_assert_true (pirwa_display_set_primary (run.display, &run.primary));
  g_signal_connect (run.window, "map", G_CALLBACK (mapped), &run);
  run.timeout = g_timeout_add_seconds (180 + run.idle_seconds, timed_out, NULL);
  gtk_window_present (run.window);
  g_main_loop_run (run.loop);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (run.window));
  GdkMonitor *monitor
      = gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface);
  g_print ("{\"width\":%d,\"height\":%d,\"mode\":\"%s\",\"warmup_frames\":%u,"
           "\"renderer\":\"%s\",\"scale\":%d,\"view_width\":%d,\"view_height\":%d,\"refresh_"
           "millihz\":%d,"
           "\"pss_start_kib\":%" G_GINT64_FORMAT ",\"pss_end_kib\":%" G_GINT64_FORMAT ","
           "\"cpu_percent_one_core\":%.6f,\"elapsed_us\":%" G_GINT64_FORMAT ","
           "\"correctness_passed\":true,\"samples\":[",
           run.width, run.height, run.mode, run.warmup,
           G_OBJECT_TYPE_NAME (gtk_native_get_renderer (GTK_NATIVE (run.window))),
           gdk_surface_get_scale_factor (surface), gtk_widget_get_width (GTK_WIDGET (run.display)),
           gtk_widget_get_height (GTK_WIDGET (run.display)),
           monitor ? gdk_monitor_get_refresh_rate (monitor) : 0, run.pss_first, run.pss_last,
           (run.sample_cpu_end - run.sample_cpu_start) * 100.0
               / (run.sample_end - run.sample_start),
           run.sample_end - run.sample_start);
  for (guint i = 0; i < run.samples->len; i++)
    {
      Sample *sample = &g_array_index (run.samples, Sample, i);
      g_print ("%s{\"paint_wall_us\":%" G_GINT64_FORMAT ",\"paint_cpu_us\":%" G_GINT64_FORMAT
               ",\"producer_cpu_us\":%" G_GINT64_FORMAT ",\"interval_us\":%" G_GINT64_FORMAT "}",
               i ? "," : "", sample->paint_wall_us, sample->paint_cpu_us, sample->producer_cpu_us,
               sample->interval_us);
    }
  g_print ("],\"idle_wall_us\":%" G_GINT64_FORMAT ",\"idle_cpu_us\":%" G_GINT64_FORMAT
           ",\"idle_paints\":%u,\"max_channel_error\":%u}\n",
           run.idle_wall_us, run.idle_cpu_us, run.idle_paints, run.max_channel_error);
  g_source_remove (run.timeout);
  g_signal_handlers_disconnect_by_data (run.clock, &run);
  g_object_unref (run.clock);
  gtk_window_destroy (run.window);
  g_array_unref (run.samples);
  g_main_loop_unref (run.loop);
  return 0;
}
