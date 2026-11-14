/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in real-guest profiling. Link wrappers instrument only this executable. */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-console.h"
#include "pirwa-display.h"
#include "pirwa-frame.h"
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <time.h>

typedef struct
{
  gint64 wall_us, cpu_us, damage_to_paint_us;
} Frame;

static GtkWindow *window;
static GMainLoop *loop;
static GdkFrameClock *frame_clock;
static GArray *frames;
static char *output, *label;
static int seconds = 10, width, height, format;
static gboolean measuring, force_full;
static guint64 invalidations, damaged_pixels, full_copies, tile_copies, copied_bytes;
static gint64 copy_cpu_us, first_damage, paint_start, paint_cpu_start, start_wall, start_cpu;
static guint64 copies_before_paint;
static guint measurement_source;
static gulong before_handler, after_handler;
static PirwaDisplay *guest_display; /* Borrowed from the window's child tree. */
static guint primary_changes;
static gboolean agent_connected;

static void
write_ready (void)
{
  if (!output || !width || !height || measuring)
    return;
  g_autofree char *path = g_build_filename (output, "ready.json", NULL);
  g_autofree char *json = g_strdup_printf ("{\"width\":%d,\"height\":%d,\"agent_connected\":%s}\n",
                                           width, height, agent_connected ? "true" : "false");
  g_assert_true (g_file_set_contents (path, json, -1, NULL));
}

static void
agent_update (SpiceMainChannel *channel, gpointer data)
{
  (void)data;
  g_object_get (channel, "agent-connected", &agent_connected, NULL);
  write_ready ();
}

static gint64
cpu_time (clockid_t id)
{
  struct timespec value;
  g_assert_cmpint (clock_gettime (id, &value), ==, 0);
  return (gint64)value.tv_sec * G_USEC_PER_SEC + value.tv_nsec / 1000;
}

static gint64
pss (void)
{
  g_autofree char *contents = NULL;
  if (!g_file_get_contents ("/proc/self/smaps_rollup", &contents, NULL, NULL))
    return -1;
  const char *line = strstr (contents, "\nPss:");
  return line ? g_ascii_strtoll (line + 5, NULL, 10) : -1;
}

SpiceSession *__real_spice_session_new (void);
void __real_pirwa_display_invalidate (PirwaDisplay *, int, int, int, int);
gboolean __real_pirwa_display_set_primary (PirwaDisplay *, const SpiceDisplayPrimary *);
GdkTexture *__real_pirwa_frame_texture (const SpiceDisplayPrimary *, GdkTexture *,
                                        cairo_region_t *);
GdkTexture *__real_pirwa_frame_texture_rect (const SpiceDisplayPrimary *,
                                             const cairo_rectangle_int_t *);

static void
channel_new (SpiceSession *session, SpiceChannel *channel, gpointer data)
{
  (void)session;
  (void)data;
  if (SPICE_IS_MAIN_CHANNEL (channel))
    g_signal_connect (channel, "main-agent-update", G_CALLBACK (agent_update), NULL);
}

SpiceAudio *
spice_audio_get (SpiceSession *session, GMainContext *context)
{
  (void)session;
  (void)context;
  /* Interpose shared-library calls too: SPICE queries agent volume information
   * even with audio channels disabled. This display fixture needs no audio. */
  return NULL;
}

SpiceSession *
__wrap_spice_session_new (void)
{
  SpiceSession *session = __real_spice_session_new ();
  /* This fixture measures display only and never opens host audio devices. */
  g_object_set (session, "enable-audio", FALSE, NULL);
  g_signal_connect (session, "channel-new", G_CALLBACK (channel_new), NULL);
  return session;
}

gboolean
__wrap_pirwa_display_set_primary (PirwaDisplay *display, const SpiceDisplayPrimary *primary)
{
  if (measuring)
    primary_changes++;
  guest_display = display;
  width = primary->width;
  height = primary->height;
  format = primary->format;
  gboolean valid = __real_pirwa_display_set_primary (display, primary);
  if (valid)
    write_ready ();
  return valid;
}

void
__wrap_pirwa_display_invalidate (PirwaDisplay *display, int x, int y, int w, int h)
{
  if (measuring && w > 0 && h > 0)
    {
      invalidations++;
      damaged_pixels += (guint64)w * h;
      if (!first_damage)
        first_damage = g_get_monotonic_time ();
    }
  /* Same transport and widget; force the original full-copy path for A/B. */
  if (force_full)
    __real_pirwa_display_invalidate (display, 0, 0, width, height);
  else
    __real_pirwa_display_invalidate (display, x, y, w, h);
}

GdkTexture *
__wrap_pirwa_frame_texture (const SpiceDisplayPrimary *primary, GdkTexture *previous,
                            cairo_region_t *damage)
{
  gint64 before = measuring ? cpu_time (CLOCK_THREAD_CPUTIME_ID) : 0;
  GdkTexture *texture = __real_pirwa_frame_texture (primary, previous, damage);
  if (measuring && texture)
    {
      full_copies++;
      copied_bytes += (guint64)primary->width * primary->height * 4;
      copy_cpu_us += cpu_time (CLOCK_THREAD_CPUTIME_ID) - before;
    }
  return texture;
}

GdkTexture *
__wrap_pirwa_frame_texture_rect (const SpiceDisplayPrimary *primary,
                                 const cairo_rectangle_int_t *rect)
{
  gint64 before = measuring ? cpu_time (CLOCK_THREAD_CPUTIME_ID) : 0;
  GdkTexture *texture = __real_pirwa_frame_texture_rect (primary, rect);
  if (measuring && texture)
    {
      tile_copies++;
      copied_bytes += (guint64)rect->width * rect->height * 4;
      copy_cpu_us += cpu_time (CLOCK_THREAD_CPUTIME_ID) - before;
    }
  return texture;
}

static void
before_paint (GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  (void)data;
  paint_start = g_get_monotonic_time ();
  paint_cpu_start = cpu_time (CLOCK_THREAD_CPUTIME_ID);
  copies_before_paint = full_copies + tile_copies;
}

static void
after_paint (GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  (void)data;
  if (!measuring || full_copies + tile_copies == copies_before_paint)
    return;
  gint64 now = g_get_monotonic_time ();
  Frame frame = { now - paint_start, cpu_time (CLOCK_THREAD_CPUTIME_ID) - paint_cpu_start,
                  first_damage ? now - first_damage : 0 };
  if (frames->len < 10000)
    g_array_append_val (frames, frame);
  first_damage = 0;
}

static gboolean
finish_measurement (gpointer data)
{
  (void)data;
  gint64 wall = g_get_monotonic_time () - start_wall;
  gint64 cpu = cpu_time (CLOCK_PROCESS_CPUTIME_ID) - start_cpu;
  measuring = FALSE;
  measurement_source = 0;
  g_autoptr (GString) json = g_string_new (NULL);
  g_string_append_printf (
      json,
      "{\"label\":\"%s\",\"force_full\":%s,\"width\":%d,\"height\":%d,\"format\":%d,"
      "\"renderer\":\"%s\",\"scale\":%d,\"wall_us\":%" G_GINT64_FORMAT
      ",\"cpu_us\":%" G_GINT64_FORMAT ",\"pss_kib\":%" G_GINT64_FORMAT
      ",\"invalidations\":%" G_GUINT64_FORMAT ",\"damaged_pixels_sum\":%" G_GUINT64_FORMAT
      ",\"full_copies\":%" G_GUINT64_FORMAT ",\"tile_copies\":%" G_GUINT64_FORMAT
      ",\"copied_bytes\":%" G_GUINT64_FORMAT ",\"copy_cpu_us\":%" G_GINT64_FORMAT
      ",\"primary_changes\":%u,\"suspended\":%s,\"view_width\":%d,\"view_height\":%d,\"samples\":[",
      label, force_full ? "true" : "false", width, height, format,
      G_OBJECT_TYPE_NAME (gtk_native_get_renderer (GTK_NATIVE (window))),
      gtk_widget_get_scale_factor (GTK_WIDGET (window)), wall, cpu, pss (), invalidations,
      damaged_pixels, full_copies, tile_copies, copied_bytes, copy_cpu_us, primary_changes,
      gtk_window_is_suspended (window) ? "true" : "false",
      gtk_widget_get_width (GTK_WIDGET (guest_display)),
      gtk_widget_get_height (GTK_WIDGET (guest_display)));
  for (guint i = 0; i < frames->len; i++)
    {
      Frame *frame = &g_array_index (frames, Frame, i);
      g_string_append_printf (
          json, "%s[%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "]", i ? "," : "",
          frame->wall_us, frame->cpu_us, frame->damage_to_paint_us);
    }
  g_string_append (json, "]}\n");
  g_autofree char *filename = g_strconcat (label, ".json", NULL);
  g_autofree char *path = g_build_filename (output, filename, NULL);
  g_assert_true (g_file_set_contents (path, json->str, json->len, NULL));
  g_print ("Completed %s: %u painted updates, %.2f%% CPU\n", label, frames->len,
           cpu * 100.0 / wall);
  return G_SOURCE_REMOVE;
}

static void
action (GSimpleAction *simple, GVariant *parameter, gpointer data)
{
  (void)data;
  const char *name = g_action_get_name (G_ACTION (simple));
  if (g_str_equal (name, "quit"))
    g_main_loop_quit (loop);
  else if (g_str_equal (name, "measure") && !measurement_source)
    {
      const char *value;
      g_variant_get (parameter, "(&sb)", &value, &force_full);
      if (!*value || strlen (value) > 80 || width < 1 || height < 1)
        return;
      for (const char *p = value; *p; p++)
        if (!g_ascii_isalnum (*p) && *p != '-')
          return;
      g_free (label);
      label = g_strdup (value);
      /* Clear measurements after renderer caches have settled in this mode. */
      g_array_set_size (frames, 0);
      invalidations = damaged_pixels = full_copies = tile_copies = copied_bytes = 0;
      copy_cpu_us = first_damage = 0;
      primary_changes = 0;
      start_wall = g_get_monotonic_time ();
      start_cpu = cpu_time (CLOCK_PROCESS_CPUTIME_ID);
      measuring = TRUE;
      measurement_source = g_timeout_add (seconds * 1000, finish_measurement, NULL);
    }
  else if (g_str_equal (name, "full") && !measurement_source)
    force_full = g_variant_get_boolean (parameter);
}

static void
mapped (GtkWidget *widget, gpointer data)
{
  (void)data;
  frame_clock = g_object_ref (gtk_widget_get_frame_clock (widget));
  before_handler = g_signal_connect (frame_clock, "before-paint", G_CALLBACK (before_paint), NULL);
  after_handler = g_signal_connect (frame_clock, "after-paint", G_CALLBACK (after_paint), NULL);
}

static gboolean
watchdog (gpointer data)
{
  (void)data;
  g_error ("Real-console profiler watchdog expired");
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  if (g_strcmp0 (g_getenv ("PIRWA_RUN_KVM_TESTS"), "1") != 0
      || g_strcmp0 (g_getenv ("PIRWA_QA_ISOLATED"), "1") != 0)
    {
      g_printerr ("Use PIRWA_RUN_KVM_TESTS=1 inside test-gnome.py for disposable guests only.\n");
      return 77;
    }
  char *uri = NULL, *uuid = NULL;
  GOptionEntry options[]
      = { { "uri", 0, 0, G_OPTION_ARG_STRING, &uri, "Explicit disposable guest connection", "URI" },
          { "uuid", 0, 0, G_OPTION_ARG_STRING, &uuid, "Disposable guest UUID", "UUID" },
          { "output", 0, 0, G_OPTION_ARG_FILENAME, &output, "New result directory", "PATH" },
          { "seconds", 0, 0, G_OPTION_ARG_INT, &seconds, "Seconds per measurement (5–30)",
            "COUNT" },
          { 0 } };
  g_autoptr (GOptionContext) context = g_option_context_new ("— native console profiling");
  g_option_context_add_main_entries (context, options, NULL);
  if (!g_option_context_parse (context, &argc, &argv, NULL) || !uri || !uuid || !output
      || !g_uuid_string_is_valid (uuid) || seconds < 5 || seconds > 30)
    return 2;
  g_autofree char *runtime_parent = g_path_get_dirname (g_get_user_runtime_dir ());
  g_autofree char *runtime_name = g_path_get_basename (runtime_parent);
  if (!g_str_has_prefix (runtime_name, "pirwa-gnome-"))
    return 2;
  /* Read-only preflight before connecting to the guest. */
  virConnectPtr connection = virConnectOpenReadOnly (uri);
  virDomainPtr domain = connection ? virDomainLookupByUUIDString (connection, uuid) : NULL;
  const char *name = domain ? virDomainGetName (domain) : NULL;
  gboolean safe = name && g_str_has_prefix (name, "pirwa-qa-") && g_uuid_string_is_valid (name + 9)
                  && virDomainIsActive (domain) == 1;
  g_autofree char *guest_name = safe ? g_strdup (name) : NULL;
  if (domain)
    virDomainFree (domain);
  if (connection)
    virConnectClose (connection);
  if (!safe || g_mkdir (output, 0700) != 0)
    return 2;
  adw_init ();
  g_autoptr (GtkApplication) application
      = gtk_application_new ("io.github.pirwa.ConsoleProfile", G_APPLICATION_DEFAULT_FLAGS);
  if (!g_application_register (G_APPLICATION (application), NULL, NULL)
      || g_application_get_is_remote (G_APPLICATION (application)))
    return 2;
  const GActionEntry entries[]
      = { { .name = "measure", .activate = action, .parameter_type = "(sb)" },
          { .name = "full", .activate = action, .parameter_type = "b" },
          { .name = "quit", .activate = action } };
  g_action_map_add_action_entries (G_ACTION_MAP (application), entries, G_N_ELEMENTS (entries),
                                   NULL);
  frames = g_array_new (FALSE, FALSE, sizeof (Frame));
  loop = g_main_loop_new (NULL, FALSE);
  window = GTK_WINDOW (adw_window_new ());
  gtk_window_set_application (window, application);
  adw_window_set_content (ADW_WINDOW (window),
                          GTK_WIDGET (pirwa_console_new (uri, uuid, guest_name)));
  g_signal_connect (window, "map", G_CALLBACK (mapped), NULL);
  gtk_window_fullscreen (window);
  gtk_window_present (window);
  guint timeout = g_timeout_add_seconds (3600, watchdog, NULL);
  g_main_loop_run (loop);
  if (measurement_source)
    g_source_remove (measurement_source);
  g_source_remove (timeout);
  if (frame_clock)
    {
      g_signal_handler_disconnect (frame_clock, before_handler);
      g_signal_handler_disconnect (frame_clock, after_handler);
      g_clear_object (&frame_clock);
    }
  gtk_window_destroy (window);
  g_array_unref (frames);
  g_main_loop_unref (loop);
  g_free (output);
  g_free (label);
  g_free (uri);
  g_free (uuid);
  return 0;
}
