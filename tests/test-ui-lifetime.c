/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-backend.h"
#include "pirwa-create-dialog.h"
#include "pirwa-window.h"
#include <glib/gstdio.h>
#include <spice-client.h>
#include <stdarg.h>

typedef struct
{
  GType type;
  guint created;
  guint finalized;
} Objects;

static GMutex lock;
static Objects objects[32];
static guint n_types;
static GPtrArray *workers;
static PirwaBackend *window_backend;
static GPtrArray *last_inventory;
static GtkSingleSelection *active_selection;
static gboolean refresh_during_edit;
static gboolean clear_edit_selection;
static guint edit_refreshes;
static guint edit_invalidations;

static void
finalized (gpointer data)
{
  Objects *counts = data;
  g_mutex_lock (&lock);
  counts->finalized++;
  g_mutex_unlock (&lock);
}

static void
observe (GObject *object)
{
  GQuark marker = g_quark_from_static_string ("pirwa-qa-finalization-marker");
  if (g_object_get_qdata (object, marker))
    return;
  GType type = G_OBJECT_TYPE (object);
  g_mutex_lock (&lock);
  guint index = 0;
  while (index < n_types && objects[index].type != type)
    index++;
  if (index == n_types)
    {
      g_assert_cmpuint (n_types, <, G_N_ELEMENTS (objects));
      objects[n_types++].type = type;
    }
  objects[index].created++;
  g_mutex_unlock (&lock);
  /* Qdata is released in GObject finalization, including after a forced widget
   * disposal. The marker holds no reference to the observed object. */
  g_object_set_qdata_full (object, marker, &objects[index], finalized);
}

gpointer
__wrap_g_object_new (GType type, const char *first_property, ...)
{
  va_list args;
  va_start (args, first_property);
  GObject *object = g_object_new_valist (type, first_property, args);
  va_end (args);
  if (g_str_has_prefix (g_type_name (type), "Pirwa"))
    observe (object);
  if (g_str_equal (g_type_name (type), "PirwaBackend") && !window_backend)
    window_backend = PIRWA_BACKEND (object);
  return object;
}

PirwaReply *__real_pirwa_backend_request_finish (PirwaBackend *backend, GAsyncResult *result,
                                                 GError **error);

PirwaReply *
__wrap_pirwa_backend_request_finish (PirwaBackend *backend, GAsyncResult *result, GError **error)
{
  PirwaReply *reply = __real_pirwa_backend_request_finish (backend, result, error);
  if (backend == window_backend && reply)
    {
      if (reply->machines)
        {
          g_clear_pointer (&last_inventory, g_ptr_array_unref);
          last_inventory = g_ptr_array_ref (reply->machines);
        }
      if (clear_edit_selection && reply->details)
        {
          clear_edit_selection = FALSE;
          edit_invalidations++;
          gtk_single_selection_set_selected (active_selection, GTK_INVALID_LIST_POSITION);
          g_assert_null (gtk_single_selection_get_selected_item (active_selection));
        }
      else if (refresh_during_edit && reply->details)
        {
          /* A same-VM inventory refresh must not lose an explicit Settings
           * request. Reuse real test-driver state and the completion serial. */
          g_assert_nonnull (last_inventory);
          g_autoptr (GPtrArray) machines = g_steal_pointer (&last_inventory);
          PirwaReply inventory = { .machines = machines, .serial = reply->serial };
          refresh_during_edit = FALSE;
          edit_refreshes++;
          g_signal_emit_by_name (backend, "inventory", &inventory);
        }
    }
  return reply;
}

GSettings *__real_g_settings_new_full (GSettingsSchema *schema, GSettingsBackend *backend,
                                       const char *path);
SpiceSession *__real_spice_session_new (void);
SpiceAudio *__real_spice_audio_get (SpiceSession *session, GMainContext *context);

GSettings *
__wrap_g_settings_new_full (GSettingsSchema *schema, GSettingsBackend *backend, const char *path)
{
  GSettings *settings = __real_g_settings_new_full (schema, backend, path);
  observe (G_OBJECT (settings));
  return settings;
}

static void
channel_created (SpiceSession *session, SpiceChannel *channel, gpointer data)
{
  (void)session;
  (void)data;
  observe (G_OBJECT (channel));
}

SpiceSession *
__wrap_spice_session_new (void)
{
  SpiceSession *session = __real_spice_session_new ();
  observe (G_OBJECT (session));
  g_signal_connect (session, "channel-new", G_CALLBACK (channel_created), NULL);
  return session;
}

SpiceAudio *
__wrap_spice_audio_get (SpiceSession *session, GMainContext *context)
{
  SpiceAudio *audio = __real_spice_audio_get (session, context);
  if (audio)
    observe (G_OBJECT (audio));
  return audio;
}

GThread *__real_g_thread_new (const char *name, GThreadFunc function, gpointer data);

GThread *
__wrap_g_thread_new (const char *name, GThreadFunc function, gpointer data)
{
  GThread *thread = __real_g_thread_new (name, function, data);
  if (g_strcmp0 (name, "pirwa-backend") == 0)
    g_ptr_array_add (workers, g_thread_ref (thread));
  return thread;
}

typedef struct
{
  GMainLoop *loop;
  GSourceFunc ready;
  gpointer data;
  const char *phase;
} Wait;

static gboolean
check_ready (gpointer data)
{
  Wait *wait = data;
  if (!wait->ready (wait->data))
    return G_SOURCE_CONTINUE;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
timed_out (gpointer data)
{
  Wait *wait = data;
  g_mutex_lock (&lock);
  for (guint i = 0; i < n_types; i++)
    g_printerr ("%s: %u created, %u finalized\n", g_type_name (objects[i].type), objects[i].created,
                objects[i].finalized);
  g_mutex_unlock (&lock);
  g_printerr ("Injected edit refreshes: %u\n", edit_refreshes);
  g_error ("UI lifetime fixture timed out waiting for %s", wait->phase);
  return G_SOURCE_REMOVE;
}

static void
wait_for (GSourceFunc ready, gpointer data, const char *phase)
{
  if (ready (data))
    return;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Wait wait = { .loop = loop, .ready = ready, .data = data, .phase = phase };
  g_timeout_add (10, check_ready, &wait);
  guint watchdog = g_timeout_add_seconds (20, timed_out, &wait);
  g_main_loop_run (loop);
  g_source_remove (watchdog);
}

#define wait_until(ready, data) wait_for (ready, data, #ready)

static gboolean
all_finalized (gpointer data)
{
  (void)data;
  gboolean done = TRUE;
  g_mutex_lock (&lock);
  for (guint i = 0; i < n_types; i++)
    done &= objects[i].created == objects[i].finalized;
  g_mutex_unlock (&lock);
  return done;
}

static gboolean
window_ready (gpointer data)
{
  return gtk_widget_get_mapped (data) && gtk_widget_get_width (data) > 0
         && g_action_group_get_action_enabled (G_ACTION_GROUP (data), "new");
}

static gboolean
console_ready (gpointer data)
{
  return g_action_group_get_action_enabled (G_ACTION_GROUP (data), "pop-out");
}

static gboolean
dialog_visible (gpointer data)
{
  AdwDialog *dialog = adw_application_window_get_visible_dialog (data);
  return dialog && gtk_widget_get_mapped (GTK_WIDGET (dialog));
}

static gboolean
dialog_closed (gpointer data)
{
  return adw_application_window_get_visible_dialog (data) == NULL;
}

static gboolean
edit_ready (gpointer data)
{
  return g_action_group_get_action_enabled (G_ACTION_GROUP (data), "edit");
}

static gboolean
edit_invalidated (gpointer data)
{
  (void)data;
  return !clear_edit_selection;
}

static gboolean
windows_closed (gpointer data)
{
  return gtk_application_get_windows (data) == NULL;
}

static void
activate (GtkWindow *window, const char *action)
{
  g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (window), action));
  g_action_group_activate_action (G_ACTION_GROUP (window), action, NULL);
}

static GtkWidget *
find_list (GtkWidget *widget)
{
  if (GTK_IS_LIST_VIEW (widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *list = find_list (child);
      if (list)
        return list;
    }
  return NULL;
}

static void
observe_models (GtkWindow *window)
{
  GtkWidget *list = find_list (GTK_WIDGET (window));
  g_assert_nonnull (list);
  GtkSelectionModel *selection = gtk_list_view_get_model (GTK_LIST_VIEW (list));
  g_assert_true (GTK_IS_SINGLE_SELECTION (selection));
  active_selection = GTK_SINGLE_SELECTION (selection);
  gtk_single_selection_set_selected (GTK_SINGLE_SELECTION (selection), 0);
  observe (G_OBJECT (selection));
  GListModel *filtered = gtk_single_selection_get_model (GTK_SINGLE_SELECTION (selection));
  g_assert_true (GTK_IS_FILTER_LIST_MODEL (filtered));
  observe (G_OBJECT (filtered));
  observe (G_OBJECT (gtk_filter_list_model_get_filter (GTK_FILTER_LIST_MODEL (filtered))));
  observe (G_OBJECT (gtk_filter_list_model_get_model (GTK_FILTER_LIST_MODEL (filtered))));
}

static void
close_dialog (GtkWindow *window, gboolean close_parent)
{
  wait_until (dialog_visible, window);
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
  if (!PIRWA_IS_CREATE_DIALOG (dialog))
    observe (G_OBJECT (dialog));
  if (close_parent)
    gtk_window_close (window);
  else
    g_assert_true (adw_dialog_close (dialog));
  wait_until (dialog_closed, window);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (window)));
}

int
main (int argc, char **argv)
{
  int cycles = 8;
  GOptionEntry entries[]
      = { { "cycles", 0, 0, G_OPTION_ARG_INT, &cycles, "Repeated ownership checks", "COUNT" },
          { NULL } };
  g_autoptr (GOptionContext) options = g_option_context_new (NULL);
  g_option_context_add_main_entries (options, entries, NULL);
  g_assert_true (g_option_context_parse (options, &argc, &argv, NULL));
  g_assert_cmpint (cycles, >=, 1);
  g_assert_cmpint (cycles, <=, 32);
  workers = g_ptr_array_new ();
  g_setenv ("GTK_A11Y", "test", TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  adw_init ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  /* The private-desktop runner can also remove this fixture after an abort. */
  g_autofree char *directory
      = g_build_filename (g_get_user_runtime_dir (), "pirwa-ui-lifetime-XXXXXX", NULL);
  g_assert_nonnull (g_mkdtemp_full (directory, 0700));
  g_autofree char *node = g_build_filename (directory, "node.xml", NULL);
  const char *xml = "<node><domain type='test'><name>Lifetime fixture</name>"
                    "<uuid>11111111-1111-4111-8111-111111111111</uuid><memory>262144</memory>"
                    "<vcpu>1</vcpu><os><type>hvm</type></os><devices>"
                    "<graphics type='spice' autoport='yes'/></devices></domain></node>";
  g_assert_true (g_file_set_contents (node, xml, -1, NULL));
  g_autofree char *uri = g_strconcat ("test://", node, NULL);
  g_autoptr (GtkApplication) application
      = gtk_application_new ("io.github.stonega.LifetimeQA", G_APPLICATION_NON_UNIQUE);
  g_assert_true (g_application_register (G_APPLICATION (application), NULL, NULL));
  for (int round = 0; round < cycles; round++)
    {
      GtkWindow *window = pirwa_window_new (application, uri);
      GObject *held = round == 0 ? g_object_ref (G_OBJECT (window)) : NULL;
      gtk_window_present (window);
      wait_until (window_ready, window);
      observe_models (window);
      wait_until (console_ready, window);
      activate (window, "preferences");
      close_dialog (window, FALSE);
      activate (window, "new");
      close_dialog (window, TRUE);
      if (round % 2 == 0)
        {
          activate (window, "shutdown");
          wait_until (edit_ready, window);
          clear_edit_selection = TRUE;
          activate (window, "edit");
          wait_until (edit_invalidated, NULL);
          g_assert_true (dialog_closed (window));
          gtk_single_selection_set_selected (active_selection, 0);
          wait_until (edit_ready, window);
          g_assert_true (dialog_closed (window));
          refresh_during_edit = TRUE;
          activate (window, "edit");
          close_dialog (window, TRUE);
          g_assert_false (refresh_during_edit);
          gtk_window_close (window);
        }
      else
        {
          activate (window, "pop-out");
          GtkWindow *popup = NULL;
          for (GList *item = gtk_application_get_windows (application); item; item = item->next)
            if (item->data != window)
              popup = item->data;
          g_assert_nonnull (popup);
          observe (G_OBJECT (popup));
          gtk_window_close (window);
          g_assert_true (gtk_widget_get_visible (GTK_WIDGET (popup)));
          gtk_window_close (popup);
        }
      wait_until (windows_closed, application);
      window_backend = NULL;
      active_selection = NULL;
      g_clear_pointer (&last_inventory, g_ptr_array_unref);
      /* Test-only joins allow completed async cleanup to be inspected. */
      for (guint i = 0; i < workers->len; i++)
        g_thread_join (g_ptr_array_index (workers, i));
      g_ptr_array_set_size (workers, 0);
      while (g_main_context_iteration (NULL, FALSE))
        ;
      if (held)
        {
          /* Disposal alone must not count as finalization while a caller still
           * owns the window. Releasing this control reference must finish it. */
          g_assert_false (all_finalized (NULL));
          g_assert_nonnull (g_object_get_qdata (
              held, g_quark_from_static_string ("pirwa-qa-finalization-marker")));
          g_clear_object (&held);
        }
      wait_until (all_finalized, NULL);
    }
  g_print ("{\"cycles\":%d,\"held_reference_control\":true,\"edit_refreshes\":%u,"
           "\"edit_invalidations\":%u,\"objects\":[",
           cycles, edit_refreshes, edit_invalidations);
  for (guint i = 0; i < n_types; i++)
    g_print ("%s{\"type\":\"%s\",\"created\":%u,\"finalized\":%u}", i ? "," : "",
             g_type_name (objects[i].type), objects[i].created, objects[i].finalized);
  g_print ("]}\n");
  g_ptr_array_unref (workers);
  g_unlink (node);
  g_rmdir (directory);
  return 0;
}
