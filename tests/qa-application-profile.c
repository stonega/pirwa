/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit LD_PRELOAD observer for the unchanged production entry point. */
#define _GNU_SOURCE
#include "qa-frame-feedback.h"
#include <dlfcn.h>
#include <string.h>

static struct
{
  gboolean enabled, finished;
  const char *output;
  GtkWindow *window;
  GdkFrameClock *clock;
  gulong map, before, after;
  guint watch, timeout;
  guint expected_domains;
  gint64 started, before_paint, first_paint;
  GArray *feedback;
} profile;

static void __attribute__ ((constructor))
initialize_profile (void)
{
  const char *uri = g_getenv ("PIRWA_LIBVIRT_URI");
  profile.output = g_getenv ("PIRWA_PROFILE_OUTPUT");
  profile.enabled = profile.output && g_strcmp0 (g_getenv ("PIRWA_QA_ISOLATED"), "1") == 0 && uri
                    && g_str_has_prefix (uri, "test:///");
  if (profile.enabled)
    {
      if (!GTK_CHECK_VERSION (4, 24, 0))
        g_error ("Production presentation profiling needs GTK 4.24+");
      profile.started = g_get_monotonic_time ();
      profile.expected_domains = g_ascii_strtoull (g_getenv ("PIRWA_PROFILE_DOMAINS"), NULL, 10);
    }
}

static GtkListView *
find_list (GtkWidget *widget)
{
  if (GTK_IS_LIST_VIEW (widget))
    {
      GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (widget));
      if (model && GTK_IS_SINGLE_SELECTION (model))
        {
          g_autoptr (GObject) item = g_list_model_get_item (G_LIST_MODEL (model), 0);
          if (!item || g_str_equal (G_OBJECT_TYPE_NAME (item), "PirwaMachine"))
            return GTK_LIST_VIEW (widget);
        }
    }
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkListView *list = find_list (child);
      if (list)
        return list;
    }
  return NULL;
}

static void
finish_profile (gboolean complete)
{
  if (profile.finished)
    return;
  profile.finished = TRUE;
  gint64 presented = qa_frame_feedback_first_presentation (profile.feedback);
  GtkListView *list = find_list (GTK_WIDGET (profile.window));
  guint count = list ? g_list_model_get_n_items (G_LIST_MODEL (gtk_list_view_get_model (list))) : 0;
  gboolean ready = G_IS_ACTION_GROUP (profile.window)
                   && g_action_group_get_action_enabled (G_ACTION_GROUP (profile.window), "new");
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (profile.window));
  GdkMonitor *monitor
      = surface ? gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface)
                : NULL;
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (profile.window));
  g_autoptr (GString) json = g_string_new (NULL);
  g_string_append_printf (json,
                          "{\"profile_complete\":%s,\"main_monotonic_us\":%" G_GINT64_FORMAT
                          ",\"first_paint_monotonic_us\":%" G_GINT64_FORMAT
                          ",\"first_presentation_monotonic_us\":%" G_GINT64_FORMAT
                          ",\"ready_monotonic_us\":%" G_GINT64_FORMAT
                          ",\"domains\":%u,\"renderer\":\"%s\","
                          "\"refresh_millihz\":%d,\"surface_scale\":%d,\"presentation_feedback\":",
                          complete ? "true" : "false", profile.started, profile.first_paint,
                          presented, ready ? g_get_monotonic_time () : 0, count,
                          renderer ? G_OBJECT_TYPE_NAME (renderer) : "unavailable",
                          monitor ? gdk_monitor_get_refresh_rate (monitor) : 0,
                          surface ? gdk_surface_get_scale_factor (surface) : 0);
  qa_frame_feedback_json (profile.feedback, json);
  g_string_append (json, "}\n");
  g_autoptr (GError) error = NULL;
  if (!g_file_set_contents (profile.output, json->str, json->len, &error))
    g_error ("Cannot save startup profile: %s", error->message);
  if (profile.watch)
    g_source_remove (profile.watch);
  if (profile.timeout)
    g_source_remove (profile.timeout);
  if (profile.clock)
    {
      g_signal_handler_disconnect (profile.clock, profile.before);
      g_signal_handler_disconnect (profile.clock, profile.after);
      g_clear_object (&profile.clock);
    }
  g_signal_handler_disconnect (profile.window, profile.map);
  g_clear_pointer (&profile.feedback, g_array_unref);
  gtk_window_close (profile.window);
  GApplication *application = g_application_get_default ();
  if (application)
    g_application_quit (application);
  g_clear_object (&profile.window);
}

static gboolean
watch_profile (gpointer data)
{
  (void)data;
  if (!qa_frame_feedback_first_presentation (profile.feedback))
    return G_SOURCE_CONTINUE;
  GtkListView *list = find_list (GTK_WIDGET (profile.window));
  if (!list || !g_action_group_get_action_enabled (G_ACTION_GROUP (profile.window), "new")
      || g_list_model_get_n_items (G_LIST_MODEL (gtk_list_view_get_model (list)))
             != profile.expected_domains)
    return G_SOURCE_CONTINUE;
  profile.watch = 0;
  finish_profile (TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean
profile_timeout (gpointer data)
{
  (void)data;
  profile.timeout = 0;
  finish_profile (FALSE);
  return G_SOURCE_REMOVE;
}

static void
before_paint (GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  (void)data;
  profile.before_paint = g_get_monotonic_time ();
}

static void
after_paint (GdkFrameClock *clock, gpointer data)
{
  (void)data;
  gint64 now = g_get_monotonic_time ();
  if (!profile.first_paint)
    profile.first_paint = now;
  if (profile.feedback->len < 10000)
    qa_frame_feedback_record (profile.feedback, clock, profile.before_paint, now, 0);
}

static void
mapped (GtkWidget *widget, gpointer data)
{
  (void)data;
  if (profile.clock)
    return;
  profile.clock = g_object_ref (gtk_widget_get_frame_clock (widget));
  profile.before
      = g_signal_connect (profile.clock, "before-paint", G_CALLBACK (before_paint), NULL);
  profile.after = g_signal_connect (profile.clock, "after-paint", G_CALLBACK (after_paint), NULL);
}

void
gtk_window_present (GtkWindow *window)
{
  static void (*present) (GtkWindow *);
  if (!present)
    {
      void *symbol = dlsym (RTLD_NEXT, "gtk_window_present");
      G_STATIC_ASSERT (sizeof present == sizeof symbol);
      if (!symbol)
        g_error ("Cannot find GTK window presentation entry point: %s", dlerror ());
      memcpy (&present, &symbol, sizeof present);
    }
  if (profile.enabled && !profile.window && !profile.finished)
    {
      profile.window = g_object_ref (window);
      profile.feedback = qa_frame_feedback_new ();
      profile.map = g_signal_connect (window, "map", G_CALLBACK (mapped), NULL);
      profile.watch = g_timeout_add (10, watch_profile, NULL);
      profile.timeout = g_timeout_add_seconds (20, profile_timeout, NULL);
    }
  present (window);
}
