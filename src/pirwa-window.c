/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-window.h"
#include "config.h"
#include "pirwa-avatar.h"
#include "pirwa-backend.h"
#include "pirwa-boxes-dialog.h"
#include "pirwa-console.h"
#include "pirwa-create-dialog.h"
#include "pirwa-model.h"
#include "pirwa-preferences.h"
#include <glib/gi18n.h>

struct _PirwaWindow
{
  AdwApplicationWindow parent_instance;
  PirwaBackend *backend;
  GSettings *preferences;
  GCancellable *lifetime;
  GCancellable *operation_cancel;
  GCancellable *detail_cancel;
  GCancellable *usage_cancel;
  PirwaUsage previous_usage;
  char *usage_uuid;
  guint usage_source;
  guint64 usage_serial;
  gboolean usage_sampling;
  gboolean usage_failed;
  GListStore *machines;
  GtkSingleSelection *selection;
  GtkWidget *list;
  GtkCustomFilter *filter;
  GtkStack *stack;
  GtkStack *viewer_stack;
  GtkBox *viewer_host;
  GtkPicture *preview;
  GtkLabel *header_state;
  AdwStatusPage *viewer_status;
  GtkWidget *avatar;
  PirwaConsole *console;
  GtkWidget *machine_controls;
  GtkWidget *start_button;
  GtkWidget *shutdown_button;
  GtkWidget *eject_button;
  GtkLabel *media_name;
  GtkWidget *restart_button;
  GtkWidget *pause_button;
  GtkWidget *resume_button;
  GtkWidget *external_button;
  GtkSpinner *loading_spinner;
  AdwToastOverlay *toasts;
  AdwStatusPage *error_page;
  GtkLabel *name;
  GtkProgressBar *start_progress;
  guint start_progress_source;
  char *starting_uuid;
  GtkLabel *state;
  GtkLabel *resources;
  GtkLabel *header_resources;
  GtkLabel *usage;
  GtkLabel *network;
  GtkLabel *stage;
  GtkProgressBar *progress;
  GtkWidget *operation_bar;
  GtkWidget *connections;
  GtkSearchBar *search_bar;
  GtkSearchEntry *search;
  GStrv pools;
  char *search_text;
  PirwaDetails *details;
  char *edit_uuid;
  gboolean busy;
  gboolean updating;
  gboolean connected;
  guint64 snapshot_serial;
  guint64 detail_serial;
  guint64 foreground_request;
  PirwaBoxesPaths *boxes_paths;
  PirwaBoxesScan *boxes_pending;
  gboolean boxes_loading;
  gboolean boxes_manual;
  gboolean boxes_remembering;
};

G_DEFINE_FINAL_TYPE (PirwaWindow, pirwa_window, ADW_TYPE_APPLICATION_WINDOW)

static void update_actions (PirwaWindow *self);
static void show_form (PirwaWindow *self, gboolean edit);
static void submit (PirwaWindow *self, const PirwaRequest *request, AdwDialog *dialog);
static void load_details (PirwaWindow *self);
static void request_details (PirwaWindow *self, gboolean edit_after);
static void open_connection (PirwaWindow *self, const char *uri);
static void open_connection_with_request (PirwaWindow *self, const char *uri,
                                          const PirwaRequest *request, AdwDialog *dialog);
static void start_boxes_scan (PirwaWindow *self, gboolean automatic);
static void offer_boxes (PirwaWindow *self);
static void show_preferences (PirwaWindow *self);

static gboolean
window_is_open (PirwaWindow *self)
{
  return self->lifetime && !g_cancellable_is_cancelled (self->lifetime);
}

static PirwaMachine *
selected (PirwaWindow *self)
{
  return self->selection ? gtk_single_selection_get_selected_item (self->selection) : NULL;
}

static void
remember_selection (PirwaWindow *self)
{
  PirwaMachine *machine = selected (self);
  if (window_is_open (self) && self->connected && machine)
    pirwa_preferences_set_selected_machine (
        self->preferences, pirwa_backend_get_uri (self->backend), pirwa_machine_get_uuid (machine));
}

static void update_usage (PirwaWindow *self);

static void
reset_usage (PirwaWindow *self)
{
  g_clear_handle_id (&self->usage_source, g_source_remove);
  if (self->usage_cancel)
    g_cancellable_cancel (self->usage_cancel);
  self->usage_serial++;
  self->usage_sampling = FALSE;
  self->usage_failed = FALSE;
  self->previous_usage = (PirwaUsage){ 0 };
  g_clear_pointer (&self->usage_uuid, g_free);
}

static gboolean
can_sample_usage (PirwaWindow *self)
{
  PirwaMachine *machine = selected (self);
  return window_is_open (self) && self->connected && machine
         && pirwa_machine_get_state_code (machine) == 1 && !self->busy
         && gtk_widget_get_mapped (GTK_WIDGET (self))
         && !gtk_window_is_suspended (GTK_WINDOW (self))
         && !adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self));
}

static char *
network_rate_text (double rate)
{
  if (rate < 1024)
    return g_strdup_printf (_ ("%.0f B/s"), rate);
  g_autofree char *size = g_format_size_full (rate >= G_MAXUINT64 ? G_MAXUINT64 : (guint64)rate,
                                              G_FORMAT_SIZE_IEC_UNITS);
  return g_strdup_printf (_ ("%s/s"), size);
}

static char *
network_usage_text (const PirwaUsage *previous, const PirwaUsage *current)
{
  double rx = 0, tx = 0;
  if (!pirwa_usage_network_rates (previous, current, &rx, &tx))
    return g_strdup (current->has_network ? _ ("NET …") : _ ("NET unavailable"));
  g_autofree char *received = network_rate_text (rx);
  g_autofree char *sent = network_rate_text (tx);
  /* Translators: arrows indicate receive and transmit; each value is a byte rate. */
  return g_strdup_printf (_ ("↓ %s ↑ %s"), received, sent);
}

static void
show_usage (PirwaWindow *self, const PirwaUsage *usage)
{
  double percent = 0;
  g_autofree char *cpu = NULL;
  if (pirwa_usage_cpu_percent (&self->previous_usage, usage, &percent))
    cpu = g_strdup_printf ("%.1f%%", percent);
  else
    cpu = g_strdup (usage->has_cpu_time ? _ ("…") : _ ("unavailable"));
  g_autofree char *ram = NULL;
  if (usage->has_memory_rss)
    {
      g_autofree char *size
          = g_format_size_full (usage->memory_rss_kib * 1024, G_FORMAT_SIZE_IEC_UNITS);
      ram = g_strdup_printf (_ ("%s RSS"), size);
    }
  else
    ram = g_strdup (_ ("unavailable"));
  g_autofree char *text = g_strdup_printf (_ ("CPU %s · RAM %s"), cpu, ram);
  gtk_label_set_text (self->usage, text);
  g_autofree char *network = network_usage_text (&self->previous_usage, usage);
  gtk_label_set_text (self->network, network);
  self->previous_usage = *usage;
}

typedef struct
{
  PirwaWindow *window;
  guint64 serial;
} UsageContext;

static void
usage_done (GObject *source, GAsyncResult *result, gpointer data)
{
  UsageContext *context = data;
  PirwaWindow *self = context->window;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &error);
  g_clear_object (&self->usage_cancel);
  if (window_is_open (self))
    {
      if (source == G_OBJECT (self->backend) && context->serial == self->usage_serial
          && can_sample_usage (self))
        {
          if (reply && reply->usage.running)
            show_usage (self, &reply->usage);
          else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              self->usage_failed = TRUE;
              gtk_label_set_text (self->usage, _ ("Usage unavailable"));
              gtk_label_set_text (self->network, _ ("NET unavailable"));
            }
        }
      update_usage (self);
    }
  g_object_unref (self);
  g_free (context);
}

static gboolean
sample_usage (gpointer data)
{
  PirwaWindow *self = data;
  self->usage_source = 0;
  if (!can_sample_usage (self) || self->usage_cancel)
    {
      update_usage (self);
      return G_SOURCE_REMOVE;
    }
  self->usage_cancel = g_cancellable_new ();
  UsageContext *context = g_new0 (UsageContext, 1);
  context->window = g_object_ref (self);
  context->serial = self->usage_serial;
  PirwaRequest request = { .operation = PIRWA_USAGE, .uuid = self->usage_uuid };
  pirwa_backend_request_async (self->backend, &request, self->usage_cancel, usage_done, context);
  return G_SOURCE_REMOVE;
}

static void
update_usage (PirwaWindow *self)
{
  if (!self->usage)
    return;
  PirwaMachine *machine = selected (self);
  gboolean running = self->connected && machine && pirwa_machine_get_state_code (machine) == 1;
  const char *uuid = running ? pirwa_machine_get_uuid (machine) : NULL;
  if (g_strcmp0 (self->usage_uuid, uuid) != 0)
    {
      reset_usage (self);
      self->usage_uuid = g_strdup (uuid);
      gtk_label_set_text (self->usage, _ ("Sampling usage…"));
      gtk_label_set_text (self->network, _ ("NET …"));
    }
  gtk_widget_set_visible (GTK_WIDGET (self->usage), running);
  gtk_widget_set_visible (GTK_WIDGET (self->network), running);
  gboolean sampling = running && can_sample_usage (self);
  if (sampling != self->usage_sampling)
    {
      g_clear_handle_id (&self->usage_source, g_source_remove);
      if (self->usage_cancel)
        g_cancellable_cancel (self->usage_cancel);
      self->usage_serial++;
      self->previous_usage = (PirwaUsage){ 0 };
      self->usage_sampling = sampling;
    }
  /* One selected VM, one request in flight, and at least two seconds between
   * samples. A slow worker cannot accumulate a queue of telemetry jobs. */
  if (sampling && !self->usage_failed && !self->usage_cancel && !self->usage_source)
    self->usage_source = g_timeout_add (2000, sample_usage, self);
}

static void
usage_visibility_changed (GObject *object, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  update_usage (PIRWA_WINDOW (object));
}

static void
window_unmapped (GtkWidget *widget, gpointer data)
{
  (void)data;
  update_usage (PIRWA_WINDOW (widget));
}

static void
toast (PirwaWindow *self, const char *message)
{
  if (window_is_open (self))
    adw_toast_overlay_add_toast (self->toasts, adw_toast_new (message));
}

static void
set_action (PirwaWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
clear_console (PirwaWindow *self)
{
  if (!self->console)
    return;
  g_signal_handlers_disconnect_by_data (self->console, self);
  if (gtk_widget_get_parent (GTK_WIDGET (self->console)) == GTK_WIDGET (self->viewer_host))
    gtk_box_remove (self->viewer_host, GTK_WIDGET (self->console));
  g_clear_object (&self->console);
}

static void
return_console (PirwaConsole *console, PirwaWindow *self)
{
  if (!window_is_open (self) || console != self->console || !pirwa_console_is_detached (console))
    return;
  pirwa_console_return (console);
  gtk_box_append (self->viewer_host, GTK_WIDGET (console));
  gtk_picture_set_paintable (self->preview, NULL);
  gtk_stack_set_visible_child_name (self->viewer_stack, "viewer");
  gtk_window_present (GTK_WINDOW (self));
  /* Refresh placement actions after the widget has its new root. */
  update_actions (self);
}

static void
update_console (PirwaWindow *self)
{
  if (!self->viewer_stack)
    return;
  PirwaMachine *machine = selected (self);
  const char *uri = self->backend ? pirwa_backend_get_uri (self->backend) : NULL;
  const char *uuid = machine ? pirwa_machine_get_uuid (machine) : NULL;
  int state = machine ? pirwa_machine_get_state_code (machine) : 0;
  gboolean stopped = state == 5 || state == 6;
  if (self->console
      && (!self->connected || !machine || !pirwa_console_matches (self->console, uri, uuid)))
    clear_console (self);
  if (!self->console && machine && self->connected)
    {
      PirwaConsole *detached
          = pirwa_console_find_detached (gtk_window_get_application (GTK_WINDOW (self)), uri, uuid);
      if (detached)
        self->console = g_object_ref (detached);
      else if (self->details && self->details->native_console && (state == 1 || state == 3))
        {
          self->console = g_object_ref_sink (
              pirwa_console_new (uri, uuid, pirwa_machine_get_title (machine)));
          gtk_box_append (self->viewer_host, GTK_WIDGET (self->console));
        }
      if (self->console)
        g_signal_connect_object (self->console, "return-requested", G_CALLBACK (return_console),
                                 self, 0);
    }
  gboolean detached = self->console && pirwa_console_is_detached (self->console);
  gtk_stack_set_visible_child_name (self->viewer_stack, stopped         ? "status"
                                                        : detached      ? "detached"
                                                        : self->console ? "viewer"
                                                                        : "status");
  gtk_picture_set_paintable (self->preview,
                             detached ? pirwa_console_get_preview (self->console) : NULL);
  set_action (self, "pop-out", self->console && !detached);
  set_action (self, "return-viewer", detached);
  set_action (self, "release-input", self->console && !detached);
}

static void
update_actions (PirwaWindow *self)
{
  if (!window_is_open (self))
    return;
  PirwaMachine *machine = selected (self);
  int state = machine ? pirwa_machine_get_state_code (machine) : 0;
  gboolean ready = machine && !self->busy && self->connected;
  gboolean stopped = state == 5 || state == 6;
  pirwa_avatar_set_running (self->avatar, machine && state == 1);
  set_action (self, "start", ready && stopped);
  set_action (self, "shutdown", ready && state == 1);
  gboolean have_media = machine && self->details && self->details->media_source;
  set_action (self, "eject", ready && have_media && (stopped || state == 1 || state == 3));
  set_action (self, "reboot", ready && state == 1);
  set_action (self, "pause", ready && state == 1);
  set_action (self, "resume", ready && state == 3);
  set_action (self, "force-stop", ready && state != 5 && state != 6 && state != 0);
  set_action (self, "console", ready && (state == 1 || state == 3));
  set_action (self, "edit", ready && self->details != NULL);
  set_action (self, "remove", ready && stopped && self->details != NULL);
  set_action (self, "machine-edit", ready && self->details != NULL);
  set_action (self, "machine-remove", ready && stopped && self->details != NULL);
  set_action (self, "machine-force-stop", ready && state != 5 && state != 6 && state != 0);
  set_action (self, "new", !self->busy && self->connected);
  set_action (self, "import-boxes",
              !self->busy && !self->boxes_loading && !self->boxes_remembering
                  && !adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self)));
  set_action (self, "search",
              self->connected && g_list_model_get_n_items (G_LIST_MODEL (self->machines)) > 0);
  set_action (self, "refresh", !self->busy);
  gtk_widget_set_sensitive (self->connections, !self->busy);
  if (machine)
    {
      g_autofree char *preset = pirwa_preferences_get_machine_avatar (
          self->preferences, pirwa_backend_get_uri (self->backend),
          pirwa_machine_get_uuid (machine));
      pirwa_avatar_set_preset (self->avatar, preset);
      gtk_label_set_text (self->name, pirwa_machine_get_title (machine));
      gtk_label_set_text (self->state, pirwa_machine_get_state (machine));
      gtk_label_set_text (self->header_state, pirwa_machine_get_state (machine));
      adw_status_page_set_description (
          self->viewer_status, stopped ? NULL
                               : self->details && !self->details->native_console
                                   ? _ ("Open this machine’s display with an external viewer.")
                                   : _ ("Preparing the machine’s display…"));
    }
  else
    {
      pirwa_avatar_set_preset (self->avatar, NULL);
      gtk_label_set_text (self->name, _ ("Select a Virtual Machine"));
      gtk_label_set_text (self->state, _ ("Choose a machine to view its controls."));
      gtk_label_set_text (self->resources, "");
      gtk_label_set_text (self->header_state, "");
      adw_status_page_set_description (self->viewer_status, NULL);
    }
  gtk_widget_set_visible (GTK_WIDGET (self->state), state != 5);
  gtk_widget_set_visible (self->machine_controls, machine != NULL && !stopped);
  gtk_widget_set_visible (
      GTK_WIDGET (self->start_progress),
      self->busy && machine && self->starting_uuid
          && g_str_equal (self->starting_uuid, pirwa_machine_get_uuid (machine)));
  gtk_widget_set_visible (GTK_WIDGET (self->header_resources),
                          self->connected && machine != NULL && !stopped);
  gtk_widget_set_visible (GTK_WIDGET (self->header_state),
                          machine != NULL && state != 1 && !stopped);
  gtk_widget_set_visible (self->start_button, stopped);
  gtk_widget_set_visible (self->shutdown_button, state == 1);
  gtk_widget_set_visible (self->eject_button, have_media);
  if (have_media)
    {
      g_autofree char *name = g_filename_display_basename (self->details->media_source);
      g_autofree char *label = g_strdup_printf (_ ("Eject %s"), name);
      gtk_label_set_text (self->media_name, name);
      gtk_widget_set_tooltip_text (self->eject_button, label);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->eject_button),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    }
  else
    gtk_label_set_text (self->media_name, "");
  gtk_widget_set_visible (self->restart_button, state == 1);
  gtk_widget_set_visible (self->pause_button, state == 1);
  gtk_widget_set_visible (self->resume_button, state == 3);
  gtk_widget_set_visible (self->external_button,
                          machine && self->details && !self->details->native_console);
  update_console (self);
  update_usage (self);
}

static gboolean
matches_search (gpointer item, gpointer user_data)
{
  PirwaWindow *self = user_data;
  if (!self->search_text || !*self->search_text)
    return TRUE;
  g_autofree char *name = g_utf8_casefold (pirwa_machine_get_name (item), -1);
  g_autofree char *title = g_utf8_casefold (pirwa_machine_get_title (item), -1);
  return strstr (name, self->search_text) != NULL || strstr (title, self->search_text) != NULL;
}

static void
search_changed (GtkSearchEntry *entry, PirwaWindow *self)
{
  g_free (self->search_text);
  self->search_text = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
search_mode_changed (GtkSearchBar *bar, GParamSpec *spec, PirwaWindow *self)
{
  (void)spec;
  if (window_is_open (self) && self->list && !gtk_search_bar_get_search_mode (bar)
      && gtk_widget_get_mapped (GTK_WIDGET (self))
      && !adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self)))
    gtk_widget_grab_focus (self->list);
}

static void
row_avatar_changed (GSettings *settings, const char *key, GtkWidget *row)
{
  (void)key;
  GtkWidget *menu = g_object_get_data (G_OBJECT (row), "menu");
  const char *uuid = g_object_get_data (G_OBJECT (menu), "machine-uuid");
  const char *uri = g_object_get_data (G_OBJECT (row), "avatar-uri");
  if (!uuid || !uri)
    return;
  g_autofree char *preset = pirwa_preferences_get_machine_avatar (settings, uri, uuid);
  pirwa_avatar_set_preset (g_object_get_data (G_OBJECT (row), "avatar"), preset);
}

static void
avatars_changed (GSettings *settings, const char *key, PirwaWindow *self)
{
  (void)settings;
  (void)key;
  update_actions (self);
}

static void
row_changed (PirwaMachine *machine, GtkWidget *row)
{
  row_avatar_changed (g_object_get_data (G_OBJECT (row), "avatar-preferences"), NULL, row);
  const char *title = pirwa_machine_get_title (machine);
  const char *name = pirwa_machine_get_name (machine);
  g_autofree char *subtitle
      = g_strcmp0 (title, name) == 0
            ? g_strdup (pirwa_machine_get_state (machine))
            : g_strdup_printf (_ ("%s · %s"), pirwa_machine_get_state (machine), name);
  gtk_label_set_text (g_object_get_data (G_OBJECT (row), "name"), title);
  GtkWidget *state = g_object_get_data (G_OBJECT (row), "state");
  gboolean running = pirwa_machine_get_state_code (machine) == 1;
  pirwa_avatar_set_running (g_object_get_data (G_OBJECT (row), "avatar"), running);
  gtk_label_set_text (GTK_LABEL (state), subtitle);
  gtk_widget_remove_css_class (state, running ? "dim-label" : "success");
  gtk_widget_add_css_class (state, running ? "success" : "dim-label");
  GtkWidget *menu = g_object_get_data (G_OBJECT (row), "menu");
  g_autofree char *label = g_strdup_printf (_ ("Options for %s"), name);
  gtk_widget_set_tooltip_text (menu, label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (menu), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
row_menu_opened (GtkMenuButton *button, GParamSpec *spec, GtkListItem *item)
{
  (void)spec;
  if (!gtk_menu_button_get_active (button))
    return;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  guint position = gtk_list_item_get_position (item);
  if (PIRWA_IS_WINDOW (root) && position != GTK_INVALID_LIST_POSITION)
    gtk_single_selection_set_selected (PIRWA_WINDOW (root)->selection, position);
}

static void
create_row_menu (GtkMenuButton *button, gpointer data)
{
  (void)data;
  const char *uuid = g_object_get_data (G_OBJECT (button), "machine-uuid");
  if (!uuid)
    return;
  g_autoptr (GMenu) menu = g_menu_new ();
  const char *labels[] = { _ ("Settings"), _ ("Remove"), _ ("Force Stop") };
  const char *actions[] = { "win.machine-edit", "win.machine-remove", "win.machine-force-stop" };
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      g_autoptr (GMenuItem) entry = g_menu_item_new (labels[i], NULL);
      g_menu_item_set_action_and_target (entry, actions[i], "s", uuid);
      g_menu_append_item (menu, entry);
    }
  gtk_menu_button_set_menu_model (button, G_MENU_MODEL (menu));
  gtk_widget_add_css_class (GTK_WIDGET (gtk_menu_button_get_popover (button)),
                            "pirwa-machine-actions");
}

static void
factory_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  PirwaWindow *self = data;
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *name = gtk_label_new (NULL);
  GtkWidget *state = gtk_label_new (NULL);
  gtk_widget_set_margin_top (row, 12);
  gtk_widget_set_margin_bottom (row, 12);
  gtk_widget_set_margin_start (row, 12);
  gtk_widget_set_margin_end (row, 12);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_xalign (GTK_LABEL (state), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (GTK_LABEL (state), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (name, "heading");
  gtk_widget_add_css_class (state, "dim-label");
  gtk_widget_set_hexpand (labels, TRUE);
  GtkWidget *avatar = pirwa_avatar_new (NULL, 40);
  gtk_box_append (GTK_BOX (row), avatar);
  gtk_box_append (GTK_BOX (labels), name);
  gtk_box_append (GTK_BOX (labels), state);
  gtk_box_append (GTK_BOX (row), labels);
  GtkWidget *menu = gtk_menu_button_new ();
  gtk_widget_add_css_class (menu, "pirwa-machine-menu");
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu), "view-more-symbolic");
  gtk_menu_button_set_has_frame (GTK_MENU_BUTTON (menu), FALSE);
  gtk_menu_button_set_create_popup_func (GTK_MENU_BUTTON (menu), create_row_menu, NULL, NULL);
  gtk_widget_set_valign (menu, GTK_ALIGN_CENTER);
  g_signal_connect_object (menu, "notify::active", G_CALLBACK (row_menu_opened), item, 0);
  gtk_box_append (GTK_BOX (row), menu);
  g_object_set_data (G_OBJECT (row), "name", name);
  g_object_set_data (G_OBJECT (row), "state", state);
  g_object_set_data (G_OBJECT (row), "menu", menu);
  g_object_set_data (G_OBJECT (row), "avatar", avatar);
  g_object_set_data_full (G_OBJECT (row), "avatar-preferences", g_object_ref (self->preferences),
                          g_object_unref);
  g_signal_connect_object (self->preferences, "changed::machine-avatars",
                           G_CALLBACK (row_avatar_changed), row, 0);
  gtk_list_item_set_child (item, row);
}

static void
factory_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  PirwaWindow *self = data;
  PirwaMachine *machine = gtk_list_item_get_item (item);
  GtkWidget *row = gtk_list_item_get_child (item);
  GtkMenuButton *menu = g_object_get_data (G_OBJECT (row), "menu");
  /* Build the popover only when opened, including for recycled list items. */
  g_object_set_data_full (G_OBJECT (menu), "machine-uuid",
                          g_strdup (pirwa_machine_get_uuid (machine)), g_free);
  g_object_set_data_full (G_OBJECT (row), "avatar-uri",
                          g_strdup (pirwa_backend_get_uri (self->backend)), g_free);
  row_changed (machine, row);
  g_signal_connect_object (machine, "changed", G_CALLBACK (row_changed), row, 0);
  g_object_set_data_full (G_OBJECT (item), "machine", g_object_ref (machine), g_object_unref);
  gtk_widget_set_tooltip_text (row, pirwa_machine_get_uuid (machine));
}

static void
factory_unbind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  PirwaMachine *machine = g_object_get_data (G_OBJECT (item), "machine");
  GtkMenuButton *menu = g_object_get_data (G_OBJECT (gtk_list_item_get_child (item)), "menu");
  gtk_menu_button_popdown (menu);
  gtk_menu_button_set_menu_model (menu, NULL);
  g_object_set_data (G_OBJECT (menu), "machine-uuid", NULL);
  if (machine)
    g_signal_handlers_disconnect_by_data (machine, gtk_list_item_get_child (item));
  g_object_set_data (G_OBJECT (item), "machine", NULL);
}

static void
apply_inventory (PirwaWindow *self, PirwaReply *reply)
{
  if (!reply->machines)
    return;
  PirwaMachine *current = selected (self);
  g_autofree char *selected_uuid
      = current ? g_strdup (pirwa_machine_get_uuid (current))
                : pirwa_preferences_get_selected_machine (self->preferences,
                                                          pirwa_backend_get_uri (self->backend));
  self->updating = TRUE;
  if (!pirwa_model_apply (self->machines, &self->snapshot_serial, reply->serial, reply->machines))
    {
      self->updating = FALSE;
      return;
    }
  self->connected = TRUE;
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
  GListModel *model = gtk_single_selection_get_model (self->selection);
  guint count = g_list_model_get_n_items (model);
  guint position = count ? 0 : GTK_INVALID_LIST_POSITION;
  if (selected_uuid)
    {
      for (guint i = 0; i < count; i++)
        {
          g_autoptr (PirwaMachine) machine = g_list_model_get_item (model, i);
          if (g_str_equal (selected_uuid, pirwa_machine_get_uuid (machine)))
            {
              position = i;
              break;
            }
        }
    }
  gtk_single_selection_set_selected (self->selection, position);
  self->updating = FALSE;
  remember_selection (self);
  if (reply->pools)
    {
      g_strfreev (self->pools);
      self->pools = g_strdupv (reply->pools);
    }
  gboolean initial = g_strcmp0 (gtk_stack_get_visible_child_name (self->stack), "loading") == 0;
  gtk_stack_set_visible_child_name (self->stack, reply->machines->len ? "machines" : "empty");
  /* Open on the collection for keyboard navigation. Focusing search here
   * would animate an unused text cursor throughout the initial idle period. */
  if (initial && reply->machines->len)
    gtk_widget_grab_focus (self->list);
  PirwaMachine *machine = selected (self);
  if (!machine || g_strcmp0 (selected_uuid, pirwa_machine_get_uuid (machine)))
    g_clear_pointer (&self->details, pirwa_details_free);
  update_actions (self);
}

static void
loading_page_changed (GObject *object, GParamSpec *spec, PirwaWindow *self)
{
  (void)object;
  (void)spec;
  if (!window_is_open (self))
    return;
  gtk_spinner_set_spinning (self->loading_spinner,
                            g_strcmp0 (gtk_stack_get_visible_child_name (self->stack), "loading")
                                == 0);
}

static void
backend_inventory (PirwaBackend *backend, PirwaReply *reply, PirwaWindow *self)
{
  if (backend == self->backend && window_is_open (self))
    {
      apply_inventory (self, reply);
      if (!self->busy)
        load_details (self);
    }
}

static void
backend_error (PirwaBackend *backend, guint64 serial, const char *message, PirwaWindow *self)
{
  if (backend != self->backend || !window_is_open (self) || serial <= self->snapshot_serial)
    return;
  self->snapshot_serial = serial;
  self->connected = FALSE;
  g_autofree char *escaped = g_markup_escape_text (message, -1);
  adw_status_page_set_description (self->error_page, escaped);
  gtk_stack_set_visible_child_name (self->stack, "error");
  update_actions (self);
}

static void
backend_progress (PirwaBackend *backend, guint64 request_id, double fraction, const char *stage,
                  PirwaWindow *self)
{
  if (backend != self->backend || !window_is_open (self) || !self->busy
      || request_id != self->foreground_request)
    return;
  gtk_progress_bar_set_fraction (self->progress, fraction);
  gtk_label_set_text (self->stage, stage);
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self));
  GtkProgressBar *progress = dialog ? g_object_get_data (G_OBJECT (dialog), "progress") : NULL;
  if (progress)
    {
      gtk_progress_bar_set_fraction (progress, fraction);
      gtk_progress_bar_set_text (progress, stage);
    }
}

typedef struct
{
  PirwaWindow *window;
  AdwDialog *dialog;
  PirwaOperation operation;
  char *uuid;
  GApplication *application;
  guint64 detail_serial;
  gboolean edit_after;
} RequestContext;

static void
context_free (RequestContext *context)
{
  if (context->application)
    {
      g_application_release (context->application);
      g_object_unref (context->application);
    }
  g_object_unref (context->window);
  g_clear_object (&context->dialog);
  g_free (context->uuid);
  g_free (context);
}

static void
viewer_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (PirwaWindow) self = data;
  g_autoptr (GError) error = NULL;
  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error)
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    toast (self, error->message);
}

static void
start_error_response (AdwAlertDialog *dialog, const char *response, PirwaWindow *self)
{
  (void)dialog;
  if (g_str_equal (response, "preferences") && window_is_open (self))
    show_preferences (self);
}

static void
show_start_error (PirwaWindow *self, const GError *error)
{
  AdwDialog *dialog = adw_alert_dialog_new (_ ("Could Not Start Virtual Machine"), error->message);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _ ("Close"));
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "preferences", _ ("Preferences"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  g_signal_connect_object (dialog, "response", G_CALLBACK (start_error_response), self, 0);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
request_done (GObject *source, GAsyncResult *result, gpointer data)
{
  RequestContext *context = data;
  PirwaWindow *self = context->window;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &error);
  if (!window_is_open (self) || source != G_OBJECT (self->backend))
    {
      context_free (context);
      return;
    }
  if (context->operation == PIRWA_DETAILS)
    {
      if (context->detail_serial != self->detail_serial)
        {
          context_free (context);
          return;
        }
      g_clear_pointer (&self->edit_uuid, g_free);
      PirwaMachine *machine = selected (self);
      if (reply && machine && g_strcmp0 (context->uuid, pirwa_machine_get_uuid (machine)) == 0)
        {
          g_clear_pointer (&self->details, pirwa_details_free);
          self->details = g_steal_pointer (&reply->details);
          g_autofree char *text = g_strdup_printf (_ ("%u CPUs · %u MiB RAM"), self->details->cpus,
                                                   self->details->memory_mb);
          gtk_label_set_text (self->resources, text);
          update_actions (self);
          if (context->edit_after
              && g_action_group_get_action_enabled (G_ACTION_GROUP (self), "edit"))
            show_form (self, TRUE);
        }
      else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          gtk_label_set_text (self->resources, _ ("Resources unavailable"));
          toast (self, error->message);
        }
      context_free (context);
      return;
    }
  self->busy = FALSE;
  g_clear_handle_id (&self->start_progress_source, g_source_remove);
  g_clear_pointer (&self->starting_uuid, g_free);
  gtk_widget_set_visible (self->operation_bar, FALSE);
  if (context->dialog)
    {
      adw_dialog_set_can_close (context->dialog, TRUE);
      if (context->operation == PIRWA_IMPORT_BOXES)
        {
          GtkWidget *cancel = g_object_get_data (G_OBJECT (context->dialog), "cancel");
          GtkWidget *rescan = g_object_get_data (G_OBJECT (context->dialog), "rescan");
          gtk_button_set_label (GTK_BUTTON (cancel), _ ("Not Now"));
          gtk_widget_set_sensitive (rescan, TRUE);
          gtk_widget_set_sensitive (g_object_get_data (G_OBJECT (context->dialog), "machines"),
                                    TRUE);
          gtk_widget_set_visible (g_object_get_data (G_OBJECT (context->dialog), "progress"),
                                  FALSE);
        }
    }
  if (!reply)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (context->operation == PIRWA_START)
            show_start_error (self, error);
          else
            toast (self, error->message);
          if (context->operation <= PIRWA_RECONNECT)
            backend_error (self->backend, pirwa_backend_result_serial (result), error->message,
                           self);
        }
      if (context->dialog)
        {
          GtkLabel *message = g_object_get_data (G_OBJECT (context->dialog), "error");
          if (message)
            {
              gtk_label_set_text (message, error->message);
              gtk_widget_set_visible (GTK_WIDGET (message), TRUE);
            }
          GtkWidget *apply = g_object_get_data (G_OBJECT (context->dialog), "apply");
          if (apply)
            gtk_widget_set_sensitive (apply, TRUE);
        }
    }
  else
    {
      if ((context->operation == PIRWA_EDIT || context->operation == PIRWA_CREATE)
          && context->dialog)
        {
          const char *uri = g_object_get_data (G_OBJECT (context->dialog), "avatar-uri");
          const char *preset = g_object_get_data (G_OBJECT (context->dialog), "avatar-id");
          const char *uuid = context->operation == PIRWA_CREATE ? reply->uuid : context->uuid;
          if (uri && uuid
              && !pirwa_preferences_set_machine_avatar (self->preferences, uri, uuid, preset))
            toast (self, _ ("The machine icon could not be saved."));
        }
      if (context->dialog)
        adw_dialog_close (context->dialog);
      if (context->operation == PIRWA_SHUTDOWN)
        toast (self, _ ("Shutdown requested. Waiting for the guest."));
      if (context->operation == PIRWA_REBOOT)
        toast (self, _ ("Restart requested. Waiting for the guest."));
      apply_inventory (self, reply);
      if (reply->native_console)
        update_console (self);
      if (reply->viewer)
        g_subprocess_wait_check_async (reply->viewer, self->lifetime, viewer_done,
                                       g_object_ref (self));
      if (reply->uuid)
        {
          gtk_editable_set_text (GTK_EDITABLE (self->search), "");
          gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
          GListModel *model = gtk_single_selection_get_model (self->selection);
          for (guint i = 0; i < g_list_model_get_n_items (model); i++)
            {
              g_autoptr (PirwaMachine) machine = g_list_model_get_item (model, i);
              if (g_str_equal (pirwa_machine_get_uuid (machine), reply->uuid))
                {
                  gtk_single_selection_set_selected (self->selection, i);
                  break;
                }
            }
          toast (self, context->operation == PIRWA_IMPORT_BOXES
                           ? _ ("Machine imported into Session. Select Start to check it. The "
                                "original stays in Boxes.")
                           : _ ("Virtual machine created and started."));
        }
      if (context->operation != PIRWA_CONSOLE)
        load_details (self);
    }
  update_actions (self);
  if (context->operation == PIRWA_EJECT && !reply)
    load_details (self);
  gboolean refresh_after_import
      = context->operation == PIRWA_IMPORT_BOXES && !reply && !self->connected;
  context_free (context);
  if (refresh_after_import)
    {
      PirwaRequest refresh = { .operation = PIRWA_REFRESH };
      submit (self, &refresh, NULL);
    }
  offer_boxes (self);
}

static gboolean
pulse_start_progress (gpointer data)
{
  PirwaWindow *self = data;
  if (gtk_widget_get_mapped (GTK_WIDGET (self->start_progress)))
    gtk_progress_bar_pulse (self->start_progress);
  return G_SOURCE_CONTINUE;
}

static void
submit (PirwaWindow *self, const PirwaRequest *request, AdwDialog *dialog)
{
  if (self->busy || !window_is_open (self))
    return;
  self->busy = TRUE;
  g_clear_object (&self->operation_cancel);
  self->operation_cancel = g_cancellable_new ();
  gtk_widget_set_visible (self->operation_bar, request->operation > PIRWA_RECONNECT
                                                   && request->operation != PIRWA_START);
  if (request->operation == PIRWA_START)
    {
      self->starting_uuid = g_strdup (request->uuid);
      gtk_progress_bar_set_fraction (self->start_progress, 0);
      gtk_progress_bar_pulse (self->start_progress);
      self->start_progress_source = g_timeout_add (100, pulse_start_progress, self);
    }
  gtk_progress_bar_set_fraction (self->progress, 0);
  gtk_label_set_text (self->stage, _ ("Working…"));
  if (dialog)
    {
      adw_dialog_set_can_close (dialog, FALSE);
      GtkWidget *apply = g_object_get_data (G_OBJECT (dialog), "apply");
      if (apply)
        gtk_widget_set_sensitive (apply, FALSE);
      if (request->operation == PIRWA_IMPORT_BOXES)
        {
          GtkWidget *cancel = g_object_get_data (G_OBJECT (dialog), "cancel");
          GtkWidget *rescan = g_object_get_data (G_OBJECT (dialog), "rescan");
          gtk_button_set_label (GTK_BUTTON (cancel), _ ("Cancel Import"));
          gtk_widget_set_sensitive (rescan, FALSE);
          gtk_widget_set_sensitive (g_object_get_data (G_OBJECT (dialog), "machines"), FALSE);
          GtkProgressBar *progress = g_object_get_data (G_OBJECT (dialog), "progress");
          gtk_progress_bar_set_fraction (progress, 0);
          gtk_progress_bar_set_text (progress, _ ("Preparing import…"));
          gtk_widget_set_visible (GTK_WIDGET (progress), TRUE);
          GtkWidget *message = g_object_get_data (G_OBJECT (dialog), "error");
          gtk_label_set_text (GTK_LABEL (message), "");
          gtk_widget_set_visible (message, FALSE);
        }
    }
  RequestContext *context = g_new0 (RequestContext, 1);
  context->window = g_object_ref (self);
  context->application
      = G_APPLICATION (g_object_ref (gtk_window_get_application (GTK_WINDOW (self))));
  g_application_hold (context->application);
  context->dialog = dialog ? g_object_ref (dialog) : NULL;
  context->operation = request->operation;
  context->uuid = g_strdup (request->uuid);
  update_actions (self);
  PirwaRequest queued = *request;
  queued.request_id = ++self->foreground_request;
  pirwa_backend_request_async (self->backend, &queued, self->operation_cancel, request_done,
                               context);
}

static void
request_details (PirwaWindow *self, gboolean edit_after)
{
  self->detail_serial++;
  if (self->detail_cancel)
    g_cancellable_cancel (self->detail_cancel);
  g_clear_object (&self->detail_cancel);
  g_clear_pointer (&self->details, pirwa_details_free);
  PirwaMachine *machine = selected (self);
  const char *uuid = machine ? pirwa_machine_get_uuid (machine) : NULL;
  /* A same-machine inventory refresh can replace an in-flight Settings read.
   * Carry the explicit opening request into that replacement, but never to a
   * different selection. Connection changes clear it independently. */
  if (edit_after || g_strcmp0 (self->edit_uuid, uuid) != 0)
    {
      g_clear_pointer (&self->edit_uuid, g_free);
      self->edit_uuid = edit_after ? g_strdup (uuid) : NULL;
    }
  if (!machine || !window_is_open (self) || !self->connected)
    {
      g_clear_pointer (&self->edit_uuid, g_free);
      update_actions (self);
      return;
    }
  self->detail_cancel = g_cancellable_new ();
  RequestContext *context = g_new0 (RequestContext, 1);
  context->window = g_object_ref (self);
  context->operation = PIRWA_DETAILS;
  context->detail_serial = self->detail_serial;
  context->edit_after = self->edit_uuid != NULL;
  context->uuid = g_strdup (pirwa_machine_get_uuid (machine));
  PirwaRequest request = { .operation = PIRWA_DETAILS, .uuid = context->uuid };
  gtk_label_set_text (self->resources, _ ("Loading resources…"));
  pirwa_backend_request_async (self->backend, &request, self->detail_cancel, request_done, context);
  update_actions (self);
}

static void
load_details (PirwaWindow *self)
{
  request_details (self, FALSE);
}

static void
selection_changed (GObject *object, GParamSpec *spec, PirwaWindow *self)
{
  (void)object;
  (void)spec;
  if (self->updating)
    return;
  remember_selection (self);
  load_details (self);
  update_actions (self);
}

static void
cancel_operation (GtkButton *button, PirwaWindow *self)
{
  (void)button;
  if (self->operation_cancel)
    g_cancellable_cancel (self->operation_cancel);
}

static void
confirm_response (AdwAlertDialog *dialog, const char *response, PirwaWindow *self)
{
  if (!g_str_equal (response, "confirm"))
    return;
  PirwaRequest request
      = { .operation = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "operation")),
          .uuid = g_object_get_data (G_OBJECT (dialog), "uuid") };
  GtkCheckButton *check = g_object_get_data (G_OBJECT (dialog), "delete");
  request.delete_disk = check && gtk_check_button_get_active (check);
  submit (self, &request, NULL);
}

static void
confirm_operation (PirwaWindow *self, PirwaOperation operation)
{
  PirwaMachine *machine = selected (self);
  if (!machine)
    return;
  gboolean remove = operation == PIRWA_REMOVE;
  AdwDialog *dialog = adw_alert_dialog_new (
      remove ? _ ("Remove Virtual Machine?") : _ ("Force Stop Virtual Machine?"),
      remove ? _ ("The VM definition will be removed. Its storage is kept unless you explicitly "
                  "choose to delete the verified Pirwa disk.")
             : _ ("Force stopping can lose unsaved work in the guest."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", _ ("Cancel"), "confirm",
                                  remove ? _ ("Remove") : _ ("Force Stop"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "confirm",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_object_set_data_full (G_OBJECT (dialog), "uuid", g_strdup (pirwa_machine_get_uuid (machine)),
                          g_free);
  g_object_set_data (G_OBJECT (dialog), "operation", GINT_TO_POINTER (operation));
  if (remove && self->details && self->details->managed)
    {
      GtkWidget *check
          = gtk_check_button_new_with_label (_ ("Also delete the verified Pirwa disk"));
      adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), check);
      g_object_set_data (G_OBJECT (dialog), "delete", check);
    }
  g_signal_connect_object (dialog, "response", G_CALLBACK (confirm_response), self, 0);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
appearance_changed (AdwComboRow *row, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  const AdwColorScheme schemes[]
      = { ADW_COLOR_SCHEME_DEFAULT, ADW_COLOR_SCHEME_FORCE_LIGHT, ADW_COLOR_SCHEME_FORCE_DARK };
  guint selected = adw_combo_row_get_selected (row);
  if (selected < G_N_ELEMENTS (schemes))
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (), schemes[selected]);
}

static void
show_preferences (PirwaWindow *self)
{
  AdwDialog *dialog = adw_preferences_dialog_new ();
  adw_dialog_set_title (dialog, _ ("Preferences"));
  adw_dialog_set_content_width (dialog, 560);
  adw_dialog_set_content_height (dialog, 600);
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_set_title (page, _ ("General"));
  adw_preferences_page_set_icon_name (page, "preferences-system-symbolic");
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (group, _ ("Appearance"));
  AdwComboRow *style = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (style), _ ("Style"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (style),
                               _ ("Applies to all Pirwa windows for this session."));
  const char *styles[] = { _ ("System"), _ ("Light"), _ ("Dark"), NULL };
  g_autoptr (GtkStringList) model = gtk_string_list_new (styles);
  adw_combo_row_set_model (style, G_LIST_MODEL (model));
  AdwColorScheme scheme = adw_style_manager_get_color_scheme (adw_style_manager_get_default ());
  adw_combo_row_set_selected (
      style, scheme == ADW_COLOR_SCHEME_FORCE_LIGHT || scheme == ADW_COLOR_SCHEME_PREFER_LIGHT ? 1
             : scheme == ADW_COLOR_SCHEME_FORCE_DARK || scheme == ADW_COLOR_SCHEME_PREFER_DARK ? 2
                                                                                               : 0);
  g_signal_connect (style, "notify::selected", G_CALLBACK (appearance_changed), NULL);
  adw_preferences_group_add (group, GTK_WIDGET (style));
  adw_preferences_page_add (page, group);
  AdwPreferencesGroup *display = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (display, _ ("Viewer"));
  AdwSwitchRow *resize = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (resize),
                                 _ ("Automatically adjust guest resolution"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (resize),
                               _ ("Match the guest resolution to the viewer when resizing. "
                                  "Requires a SPICE guest agent."));
  g_autoptr (GSettings) preferences = pirwa_preferences_new ();
  g_settings_bind (preferences, "auto-resize-display", resize, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (display, GTK_WIDGET (resize));
  adw_preferences_page_add (page, display);
  AdwPreferencesGroup *network = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (network, _ ("Network"));
  AdwSwitchRow *proxy = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (proxy), _ ("Use host proxy"));
  adw_action_row_set_subtitle (
      ADW_ACTION_ROW (proxy),
      _ ("Use the manual desktop proxy for Session machines without guest setup. "
         "Applies on the next start. Supports IPv4 TCP and DNS; other UDP is blocked."));
  g_settings_bind (preferences, "use-host-proxy", proxy, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (network, GTK_WIDGET (proxy));
  adw_preferences_page_add (page, network);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), page);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
action_activated (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  PirwaWindow *self = data;
  const char *name = g_action_get_name (G_ACTION (action));
  if (g_str_equal (name, "preferences"))
    {
      show_preferences (self);
      return;
    }
  if (g_str_equal (name, "about"))
    {
      adw_show_about_dialog (
          GTK_WIDGET (self), "application-name", "Pirwa", "application-icon", PIRWA_APPLICATION_ID,
          "developer-name", _ ("Pirwa Contributors"), "version", PIRWA_VERSION, "comments",
          _ ("Manage local virtual machines"), "license-type", GTK_LICENSE_GPL_3_0, NULL);
      return;
    }
  if (g_str_has_prefix (name, "machine-"))
    {
      PirwaMachine *machine = selected (self);
      if (machine && parameter
          && g_str_equal (g_variant_get_string (parameter, NULL), pirwa_machine_get_uuid (machine))
          && g_action_group_get_action_enabled (G_ACTION_GROUP (self), name + 8))
        g_action_group_activate_action (G_ACTION_GROUP (self), name + 8, NULL);
      return;
    }
  if (g_str_equal (name, "pop-out"))
    {
      if (self->console && !pirwa_console_is_detached (self->console))
        {
          gtk_box_remove (self->viewer_host, GTK_WIDGET (self->console));
          pirwa_console_pop_out (self->console, gtk_window_get_application (GTK_WINDOW (self)));
          update_actions (self);
        }
      return;
    }
  if (g_str_equal (name, "return-viewer"))
    {
      if (self->console)
        return_console (self->console, self);
      return;
    }
  if (g_str_equal (name, "release-input"))
    {
      if (self->console)
        pirwa_console_release_input (self->console);
      return;
    }
  if (g_str_equal (name, "console") && self->console)
    {
      if (pirwa_console_is_detached (self->console))
        return_console (self->console, self);
      else
        gtk_widget_grab_focus (GTK_WIDGET (self->console));
      return;
    }
  if (g_str_equal (name, "import-boxes"))
    {
      start_boxes_scan (self, FALSE);
      return;
    }
  if (g_str_equal (name, "search"))
    {
      gtk_search_bar_set_search_mode (self->search_bar, TRUE);
      gtk_widget_grab_focus (GTK_WIDGET (self->search));
      return;
    }
  if (g_str_equal (name, "new"))
    {
      show_form (self, FALSE);
      return;
    }
  if (g_str_equal (name, "edit"))
    {
      request_details (self, TRUE);
      return;
    }
  if (g_str_equal (name, "remove"))
    {
      confirm_operation (self, PIRWA_REMOVE);
      return;
    }
  if (g_str_equal (name, "force-stop"))
    {
      confirm_operation (self, PIRWA_FORCE_STOP);
      return;
    }
  if (g_str_equal (name, "eject"))
    {
      PirwaMachine *machine = selected (self);
      if (machine && self->details && self->details->media_source)
        {
          PirwaRequest request = { .operation = PIRWA_EJECT,
                                   .uuid = pirwa_machine_get_uuid (machine),
                                   .media_source = self->details->media_source,
                                   .media_target = self->details->media_target };
          submit (self, &request, NULL);
        }
      return;
    }
  const char *names[] = { "refresh", "start", "shutdown", "reboot", "pause", "resume", "console" };
  const PirwaOperation operations[] = { PIRWA_RECONNECT, PIRWA_START,  PIRWA_SHUTDOWN, PIRWA_REBOOT,
                                        PIRWA_PAUSE,     PIRWA_RESUME, PIRWA_CONSOLE };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_str_equal (name, names[i]))
      {
        PirwaMachine *machine = selected (self);
        PirwaRequest request = { .operation = operations[i],
                                 .uuid = machine ? pirwa_machine_get_uuid (machine) : NULL };
        if (operations[i] == PIRWA_RECONNECT)
          reset_usage (self);
        submit (self, &request, NULL);
        return;
      }
}

typedef struct
{
  PirwaWindow *window;
  AdwDialog *dialog;
  AdwEntryRow *name;
  AdwSpinRow *cpus;
  AdwSpinRow *memory;
  GtkCheckButton *eject;
  GtkWidget *avatar_picker;
  GtkLabel *error;
  char *uuid;
  char *uri;
  char *original_name;
  guint original_cpus;
  guint original_memory;
} Form;

static void
form_free (gpointer data)
{
  Form *form = data;
  g_object_unref (form->window);
  g_free (form->uuid);
  g_free (form->uri);
  g_free (form->original_name);
  g_free (form);
}

static void
form_apply (GtkButton *button, Form *form)
{
  (void)button;
  if (!window_is_open (form->window) || form->window->busy)
    return;
  PirwaRequest request = { .operation = PIRWA_EDIT, .uuid = form->uuid };
  request.options.name = (char *)gtk_editable_get_text (GTK_EDITABLE (form->name));
  request.options.cpus = adw_spin_row_get_value (form->cpus);
  request.options.memory_mb = adw_spin_row_get_value (form->memory);
  request.options.disk_gb = 4;
  request.eject = form->eject && gtk_check_button_get_active (form->eject);
  g_autoptr (GError) error = NULL;
  if (!pirwa_config_validate (&request.options, &error))
    {
      gtk_label_set_text (form->error, error->message);
      gtk_widget_set_visible (GTK_WIDGET (form->error), TRUE);
      return;
    }
  const char *preset = pirwa_avatar_picker_get_preset (form->avatar_picker);
  g_autofree char *previous
      = pirwa_preferences_get_machine_avatar (form->window->preferences, form->uri, form->uuid);
  gboolean avatar_changed
      = g_strcmp0 (previous, g_str_equal (preset, "generic") ? NULL : preset) != 0;
  if ((avatar_changed && !g_settings_is_writable (form->window->preferences, "machine-avatars"))
      || g_strcmp0 (form->uri, pirwa_backend_get_uri (form->window->backend)) != 0)
    {
      gtk_label_set_text (form->error, _ ("The machine icon could not be saved."));
      gtk_widget_set_visible (GTK_WIDGET (form->error), TRUE);
      return;
    }
  if (g_str_equal (request.options.name, form->original_name)
      && request.options.cpus == form->original_cpus
      && request.options.memory_mb == form->original_memory && !request.eject)
    {
      if (pirwa_preferences_set_machine_avatar (form->window->preferences, form->uri, form->uuid,
                                                preset))
        adw_dialog_close (form->dialog);
      else
        {
          gtk_label_set_text (form->error, _ ("The machine icon could not be saved."));
          gtk_widget_set_visible (GTK_WIDGET (form->error), TRUE);
        }
      return;
    }
  /* Commit the local choice only after the VM edit succeeds. */
  g_object_set_data_full (G_OBJECT (form->dialog), "avatar-uri", g_strdup (form->uri), g_free);
  g_object_set_data_full (G_OBJECT (form->dialog), "avatar-id", g_strdup (preset), g_free);
  submit (form->window, &request, form->dialog);
}

static void
form_cancel (GtkButton *button, Form *form)
{
  (void)button;
  if (form->window->busy)
    cancel_operation (NULL, form->window);
  else
    adw_dialog_close (form->dialog);
}

static AdwSpinRow *
spin_row (GtkWidget *group, const char *title, double value, double lower, double upper,
          double step)
{
  GtkWidget *row = adw_spin_row_new_with_range (lower, upper, step);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_spin_row_set_value (ADW_SPIN_ROW (row), value);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  return ADW_SPIN_ROW (row);
}

static void
create_requested (PirwaCreateDialog *dialog, PirwaCreate *options, PirwaWindow *self)
{
  g_object_set_data_full (G_OBJECT (dialog), "avatar-uri",
                          g_strdup (pirwa_backend_get_uri (self->backend)), g_free);
  g_object_set_data_full (G_OBJECT (dialog), "avatar-id",
                          g_strdup (pirwa_create_dialog_get_avatar (dialog)), g_free);
  PirwaRequest request = { .operation = PIRWA_CREATE, .options = *options };
  submit (self, &request, ADW_DIALOG (dialog));
}

static void
create_cancelled (PirwaCreateDialog *dialog, PirwaWindow *self)
{
  (void)dialog;
  cancel_operation (NULL, self);
}

static void
show_form (PirwaWindow *self, gboolean edit)
{
  if (self->busy || (edit && !self->details))
    return;
  if (!edit)
    {
      AdwDialog *dialog = pirwa_create_dialog_new (
          (const char *const *)self->pools,
          g_str_equal (pirwa_backend_get_uri (self->backend), "qemu:///session"));
      g_signal_connect_object (dialog, "create-requested", G_CALLBACK (create_requested), self, 0);
      g_signal_connect_object (dialog, "cancel-requested", G_CALLBACK (create_cancelled), self, 0);
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }
  Form *form = g_new0 (Form, 1);
  form->window = g_object_ref (self);
  form->uuid = g_strdup (pirwa_machine_get_uuid (selected (self)));
  form->uri = g_strdup (pirwa_backend_get_uri (self->backend));
  form->original_name = g_strdup (self->details->name);
  form->original_cpus = self->details->cpus;
  form->original_memory = self->details->memory_mb;
  int state = pirwa_machine_get_state_code (selected (self));
  gboolean stopped = state == 5 || state == 6;
  form->dialog = adw_dialog_new ();
  adw_dialog_set_title (form->dialog, _ ("Virtual Machine Settings"));
  adw_dialog_set_content_width (form->dialog, 640);
  adw_dialog_set_content_height (form->dialog, 600);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_margin_top (box, 18);
  gtk_widget_set_margin_bottom (box, 18);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), box);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scrolled);
  GtkWidget *appearance = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (appearance), _ ("Machine Icon"));
  g_autofree char *preset
      = pirwa_preferences_get_machine_avatar (self->preferences, form->uri, form->uuid);
  form->avatar_picker = pirwa_avatar_picker_new (preset);
  gtk_widget_set_sensitive (form->avatar_picker,
                            g_settings_is_writable (self->preferences, "machine-avatars"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (appearance), form->avatar_picker);
  gtk_box_append (GTK_BOX (box), appearance);
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _ ("Resources"));
  if (!stopped)
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                           _ ("Power off the machine to change its resources."));
  gtk_widget_set_sensitive (group, stopped);
  gtk_box_append (GTK_BOX (box), group);
  form->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (form->name), _ ("Name"));
  gtk_editable_set_text (GTK_EDITABLE (form->name), self->details->name);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (form->name));
  form->cpus = spin_row (group, _ ("Virtual CPUs"), self->details->cpus, 1, 64, 1);
  form->memory = spin_row (group, _ ("Memory (MiB)"), self->details->memory_mb, 256, 262144, 256);
  if (stopped && self->details->has_cdrom)
    {
      form->eject
          = GTK_CHECK_BUTTON (gtk_check_button_new_with_label (_ ("Eject installation media")));
      gtk_box_append (GTK_BOX (box), GTK_WIDGET (form->eject));
    }
  form->error = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (form->error, TRUE);
  gtk_label_set_wrap_mode (form->error, PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (form->error), "error");
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (footer, 18);
  gtk_widget_set_margin_end (footer, 18);
  gtk_widget_set_visible (GTK_WIDGET (form->error), FALSE);
  gtk_box_append (GTK_BOX (footer), GTK_WIDGET (form->error));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), footer);
  g_object_set_data (G_OBJECT (form->dialog), "error", form->error);
  GtkWidget *progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *progress_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (progress_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (progress_label), PANGO_WRAP_WORD_CHAR);
  GtkWidget *progress_bar = gtk_progress_bar_new ();
  g_object_bind_property (self->stage, "label", progress_label, "label", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->progress, "fraction", progress_bar, "fraction",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->operation_bar, "visible", progress_box, "visible",
                          G_BINDING_SYNC_CREATE);
  gtk_box_append (GTK_BOX (progress_box), progress_label);
  gtk_box_append (GTK_BOX (progress_box), progress_bar);
  gtk_box_append (GTK_BOX (footer), progress_box);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Cancel"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (form_cancel), form);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *apply = gtk_button_new_with_label (_ ("Save Changes"));
  gtk_widget_add_css_class (apply, "suggested-action");
  gtk_widget_set_halign (apply, GTK_ALIGN_END);
  g_signal_connect (apply, "clicked", G_CALLBACK (form_apply), form);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply);
  g_object_set_data (G_OBJECT (form->dialog), "apply", apply);
  g_object_set_data_full (G_OBJECT (form->dialog), "form", form, form_free);
  adw_dialog_set_child (form->dialog, toolbar);
  adw_dialog_present (form->dialog, GTK_WIDGET (self));
}

static void
boxes_scan_done (GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr (PirwaWindow) self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan_finish (result, &error);
  if (!window_is_open (self))
    return;
  self->boxes_loading = FALSE;
  g_clear_pointer (&self->boxes_pending, pirwa_boxes_scan_free);
  if (scan)
    self->boxes_pending = g_steal_pointer (&scan);
  else if (self->boxes_manual && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    toast (self, error->message);
  update_actions (self);
  offer_boxes (self);
}

static void
start_boxes_scan (PirwaWindow *self, gboolean automatic)
{
  if (!window_is_open (self) || self->boxes_loading || self->boxes_remembering)
    return;
  self->boxes_manual = !automatic;
  self->boxes_loading = TRUE;
  pirwa_boxes_scan_async (self->boxes_paths, automatic, self->lifetime, boxes_scan_done,
                          g_object_ref (self));
  update_actions (self);
}

typedef struct
{
  PirwaWindow *window;
  GApplication *application;
} RememberContext;

static void
boxes_remember_done (GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  RememberContext *context = data;
  g_autoptr (GError) error = NULL;
  if (!pirwa_boxes_remember_finish (result, &error))
    toast (context->window, error->message);
  context->window->boxes_remembering = FALSE;
  update_actions (context->window);
  g_application_release (context->application);
  g_object_unref (context->application);
  g_object_unref (context->window);
  g_free (context);
}

static void
boxes_response (GtkWindow *window, AdwDialog *dialog, PirwaBoxesResponse response, const char *uri,
                const char *uuid)
{
  PirwaWindow *self = PIRWA_WINDOW (window);
  if (!window_is_open (self))
    return;
  if (response == PIRWA_BOXES_RESCAN)
    {
      start_boxes_scan (self, FALSE);
      return;
    }
  if (response == PIRWA_BOXES_CANCEL)
    {
      if (self->operation_cancel)
        g_cancellable_cancel (self->operation_cancel);
      return;
    }
  if (response == PIRWA_BOXES_IMPORT)
    {
      if (!uri || !uuid || self->busy)
        return;
      PirwaRequest request = { .operation = PIRWA_IMPORT_BOXES, .uuid = uuid, .source_uri = uri };
      self->updating = TRUE;
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->connections),
                                  g_str_has_prefix (self->boxes_paths->native_uri, "test:///") ? 2
                                                                                               : 0);
      self->updating = FALSE;
      if (g_strcmp0 (pirwa_backend_get_uri (self->backend), self->boxes_paths->native_uri))
        open_connection_with_request (self, self->boxes_paths->native_uri, &request, dialog);
      else
        submit (self, &request, dialog);
      return;
    }
  RememberContext *context = g_new0 (RememberContext, 1);
  self->boxes_remembering = TRUE;
  update_actions (self);
  context->window = g_object_ref (self);
  context->application = g_object_ref (G_APPLICATION (gtk_window_get_application (window)));
  g_application_hold (context->application);
  pirwa_boxes_remember_async (self->boxes_paths, boxes_remember_done, context);
}

static void
offer_boxes (PirwaWindow *self)
{
  if (!window_is_open (self) || !self->boxes_pending || self->busy || self->boxes_loading
      || !gtk_widget_get_mapped (GTK_WIDGET (self))
      || adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self)))
    return;
  g_autoptr (PirwaBoxesScan) scan = g_steal_pointer (&self->boxes_pending);
  if (!self->boxes_manual && (scan->already_seen || !scan->installed))
    return;
  if (scan->installed || self->boxes_manual)
    pirwa_boxes_dialog_present (GTK_WINDOW (self), g_steal_pointer (&scan), boxes_response);
}

static void
dialog_changed (GObject *object, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  PirwaWindow *self = PIRWA_WINDOW (object);
  update_actions (self);
  offer_boxes (self);
}

static void
window_mapped (GtkWidget *widget, gpointer data)
{
  (void)data;
  offer_boxes (PIRWA_WINDOW (widget));
  update_usage (PIRWA_WINDOW (widget));
}

static void
open_connection_with_request (PirwaWindow *self, const char *uri, const PirwaRequest *request,
                              AdwDialog *dialog)
{
  g_clear_pointer (&self->edit_uuid, g_free);
  reset_usage (self);
  clear_console (self);
  self->connected = FALSE;
  self->snapshot_serial = 0;
  if (self->backend)
    {
      pirwa_backend_stop (self->backend);
      g_signal_handlers_disconnect_by_data (self->backend, self);
      g_clear_object (&self->backend);
    }
  if (self->detail_cancel)
    g_cancellable_cancel (self->detail_cancel);
  g_clear_pointer (&self->details, pirwa_details_free);
  g_list_store_remove_all (self->machines);
  self->backend = pirwa_backend_new (uri);
  g_signal_connect_object (self->backend, "inventory", G_CALLBACK (backend_inventory), self, 0);
  g_signal_connect_object (self->backend, "error", G_CALLBACK (backend_error), self, 0);
  g_signal_connect_object (self->backend, "progress", G_CALLBACK (backend_progress), self, 0);
  gtk_stack_set_visible_child_name (self->stack, "loading");
  submit (self, request, dialog);
}

static void
open_connection (PirwaWindow *self, const char *uri)
{
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  open_connection_with_request (self, uri, &request, NULL);
}

static void
connection_changed (GtkDropDown *dropdown, GParamSpec *spec, PirwaWindow *self)
{
  (void)spec;
  if (self->updating || self->busy)
    return;
  guint choice = gtk_drop_down_get_selected (dropdown);
  open_connection (self, choice == 2   ? "test:///default"
                         : choice == 1 ? "qemu:///system"
                                       : "qemu:///session");
}

static void
cancel_window_work (PirwaWindow *self)
{
  g_clear_pointer (&self->edit_uuid, g_free);
  g_clear_handle_id (&self->start_progress_source, g_source_remove);
  g_clear_pointer (&self->starting_uuid, g_free);
  reset_usage (self);
  clear_console (self);
  if (self->operation_cancel)
    g_cancellable_cancel (self->operation_cancel);
  if (self->detail_cancel)
    g_cancellable_cancel (self->detail_cancel);
  if (self->lifetime)
    g_cancellable_cancel (self->lifetime);
  if (self->backend)
    pirwa_backend_stop (self->backend);
}

static gboolean
window_close_requested (GtkWindow *window, gpointer data)
{
  (void)data;
  PirwaWindow *self = PIRWA_WINDOW (window);
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self));
  if (dialog)
    {
      adw_dialog_close (dialog);
      return TRUE;
    }
  cancel_window_work (self);
  return FALSE;
}

static void
pirwa_window_dispose (GObject *object)
{
  PirwaWindow *self = PIRWA_WINDOW (object);
  cancel_window_work (self);
  g_clear_object (&self->backend);
  g_clear_object (&self->lifetime);
  g_clear_object (&self->operation_cancel);
  g_clear_object (&self->detail_cancel);
  g_clear_object (&self->usage_cancel);
  g_clear_object (&self->preferences);
  g_clear_object (&self->selection);
  g_clear_object (&self->machines);
  G_OBJECT_CLASS (pirwa_window_parent_class)->dispose (object);
}

static void
pirwa_window_finalize (GObject *object)
{
  PirwaWindow *self = PIRWA_WINDOW (object);
  g_free (self->search_text);
  g_free (self->edit_uuid);
  g_strfreev (self->pools);
  pirwa_details_free (self->details);
  pirwa_boxes_paths_free (self->boxes_paths);
  pirwa_boxes_scan_free (self->boxes_pending);
  G_OBJECT_CLASS (pirwa_window_parent_class)->finalize (object);
}

static void
pirwa_window_class_init (PirwaWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_window_dispose;
  G_OBJECT_CLASS (klass)->finalize = pirwa_window_finalize;
}

static GtkWidget *
action_button (const char *label, const char *action)
{
  GtkWidget *button = gtk_button_new_with_label (label);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
  return button;
}

static void
connection_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child (item, label);
}

static void
connection_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  GtkStringObject *choice = gtk_list_item_get_item (item);
  GtkWidget *label = gtk_list_item_get_child (item);
  gtk_label_set_text (GTK_LABEL (label), gtk_string_object_get_string (choice));
  gtk_widget_set_tooltip_text (label, gtk_string_object_get_string (choice));
}

static void
measure_header_reading (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                        int *natural, int *minimum_baseline, int *natural_baseline)
{
  GTK_WIDGET_GET_CLASS (widget)->measure (widget, orientation, for_size, minimum, natural,
                                          minimum_baseline, natural_baseline);
  /* GtkLabel normally reserves an ellipsis. Readings may disappear entirely
   * when large text leaves room only for the machine controls. */
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *minimum = 0;
}

static void
pirwa_window_init (PirwaWindow *self)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (!g_object_get_data (G_OBJECT (display), "pirwa-collection-styles"))
    {
      gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (display),
                                        "/io/github/stonega/Pirwa/icons");
      g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (provider,
                                         "paned.pirwa-machines > separator {"
                                         " background: none; border: none; box-shadow: none; }"
                                         ".pirwa-sidebar { background-color: @sidebar_bg_color;"
                                         " color: @sidebar_fg_color; }"
                                         ".pirwa-sidebar:backdrop {"
                                         " background-color: @sidebar_backdrop_color; }"
                                         ".pirwa-machine-menu { opacity: 0; }"
                                         "row:hover .pirwa-machine-menu,"
                                         "row:selected .pirwa-machine-menu,"
                                         "row:focus-within .pirwa-machine-menu { opacity: 1; }"
                                         ".pirwa-machine-actions modelbutton:nth-child(2)"
                                         ":not(:disabled) { color: @destructive_color; }"
                                         "searchbar.pirwa-search > revealer > box { padding: 0; }"
                                         "button.pirwa-shutdown {"
                                         " background-color: @warning_bg_color;"
                                         " color: @warning_fg_color; }");
      gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_set_data (G_OBJECT (display), "pirwa-collection-styles", GINT_TO_POINTER (1));
    }
  self->lifetime = g_cancellable_new ();
  self->preferences = pirwa_preferences_new ();
  g_signal_connect_object (self->preferences, "changed::machine-avatars",
                           G_CALLBACK (avatars_changed), self, 0);
  self->machines = g_list_store_new (PIRWA_TYPE_MACHINE);
  const char *actions[] = { "new",          "refresh",       "start",          "shutdown",
                            "reboot",       "pause",         "resume",         "force-stop",
                            "console",      "edit",          "remove",         "search",
                            "import-boxes", "machine-edit",  "machine-remove", "machine-force-stop",
                            "pop-out",      "return-viewer", "release-input",  "preferences",
                            "about",        "eject" };
  for (guint i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      GSimpleAction *action = g_simple_action_new (
          actions[i], g_str_has_prefix (actions[i], "machine-") ? G_VARIANT_TYPE_STRING : NULL);
      g_signal_connect (action, "activate", G_CALLBACK (action_activated), self);
      g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
      g_object_unref (action);
    }
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *sidebar_toolbar = adw_toolbar_view_new ();
  gtk_widget_add_css_class (sidebar_toolbar, "pirwa-sidebar");
  GtkWidget *content_toolbar = adw_toolbar_view_new ();
  GtkWidget *sidebar_header = adw_header_bar_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (sidebar_header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (sidebar_header),
                                   gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  g_autoptr (GtkSizeGroup) headers = gtk_size_group_new (GTK_SIZE_GROUP_VERTICAL);
  gtk_size_group_add_widget (headers, sidebar_header);
  gtk_size_group_add_widget (headers, header);
  GtkWidget *header_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (header_row, TRUE);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), header_row);
  self->header_state = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (self->header_state, 1);
  gtk_label_set_ellipsize (self->header_state, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->header_state), "dim-label");
  GtkWidget *header_info = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign (header_info, GTK_ALIGN_CENTER);
  g_object_bind_property (self->header_state, "label", self->header_state, "tooltip-text",
                          G_BINDING_SYNC_CREATE);
  GtkWidget *new_button = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_valign (new_button, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (new_button), "win.new");
  gtk_widget_set_tooltip_text (new_button, _ ("New Virtual Machine"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (new_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("New Virtual Machine"), -1);
  gtk_widget_add_css_class (new_button, "suggested-action");
  adw_header_bar_pack_start (ADW_HEADER_BAR (sidebar_header), new_button);
  self->machine_controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  const char *control_labels[] = { _ ("Shut Down"),
                                   _ ("Restart"),
                                   _ ("Pause"),
                                   _ ("Resume"),
                                   _ ("Open Console"),
                                   _ ("Release Input"),
                                   _ ("Open in Separate Window") };
  const char *control_actions[] = { "win.shutdown", "win.reboot",        "win.pause",  "win.resume",
                                    "win.console",  "win.release-input", "win.pop-out" };
  const char *control_icons[] = { "system-shutdown-symbolic",
                                  "system-reboot-symbolic",
                                  "media-playback-pause-symbolic",
                                  "media-playback-start-symbolic",
                                  "video-display-symbolic",
                                  "input-keyboard-symbolic",
                                  "window-new-symbolic" };
  for (guint i = 0; i < G_N_ELEMENTS (control_labels); i++)
    {
      GtkWidget *button = i == 0 ? gtk_button_new_with_label (control_labels[i])
                                 : gtk_button_new_from_icon_name (control_icons[i]);
      if (i == 0)
        {
          gtk_label_set_ellipsize (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (button))),
                                   PANGO_ELLIPSIZE_END);
          gtk_widget_add_css_class (button, "opaque");
          gtk_widget_add_css_class (button, "pirwa-shutdown");
          gtk_widget_set_margin_end (button, 6);
        }
      gtk_actionable_set_action_name (GTK_ACTIONABLE (button), control_actions[i]);
      gtk_widget_set_tooltip_text (button, control_labels[i]);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      control_labels[i], -1);
      gtk_box_append (GTK_BOX (self->machine_controls), button);
      if (i == 0)
        self->shutdown_button = button;
      else if (i == 1)
        self->restart_button = button;
      else if (i == 2)
        self->pause_button = button;
      else if (i == 3)
        self->resume_button = button;
      else if (i == 4)
        self->external_button = button;
    }
  self->eject_button = gtk_button_new ();
  GtkWidget *media = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->media_name = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_ellipsize (self->media_name, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->media_name, 18);
  gtk_box_append (GTK_BOX (media), GTK_WIDGET (self->media_name));
  gtk_box_append (GTK_BOX (media), gtk_image_new_from_icon_name ("media-eject-symbolic"));
  gtk_button_set_child (GTK_BUTTON (self->eject_button), media);
  gtk_widget_add_css_class (self->eject_button, "text-button");
  gtk_widget_set_margin_end (self->eject_button, 6);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (self->eject_button), "win.eject");
  gtk_box_insert_child_after (GTK_BOX (self->machine_controls), self->eject_button,
                              self->shutdown_button);
  gtk_widget_set_valign (self->machine_controls, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (self->machine_controls, 6);
  gtk_widget_set_halign (self->machine_controls, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (header_row), self->machine_controls);
  GtkWidget *header_space = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (header_space, TRUE);
  gtk_box_append (GTK_BOX (header_row), header_space);
  gtk_box_append (GTK_BOX (header_row), header_info);
  /* Include window buttons in the row so title centering cannot inset controls. */
  GtkWidget *window_controls = gtk_window_controls_new (GTK_PACK_END);
  gtk_widget_set_margin_start (window_controls, 6);
  gtk_box_append (GTK_BOX (header_row), window_controls);
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) connection_section = g_menu_new ();
  g_autoptr (GMenuItem) connection_item = g_menu_item_new (NULL, NULL);
  g_menu_item_set_attribute (connection_item, "custom", "s", "connection");
  g_menu_append_item (connection_section, connection_item);
  g_menu_append_section (menu, NULL, G_MENU_MODEL (connection_section));
  g_autoptr (GMenu) library_section = g_menu_new ();
  g_menu_append (library_section, _ ("Import from Boxes Flatpak"), "win.import-boxes");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (library_section));
  g_autoptr (GMenu) application_section = g_menu_new ();
  g_menu_append (application_section, _ ("Preferences"), "win.preferences");
  g_menu_append (application_section, _ ("About Pirwa"), "win.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (application_section));
  GtkWidget *menu_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), G_MENU_MODEL (menu));
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_button), TRUE);
  gtk_widget_set_tooltip_text (menu_button, _ ("Main Menu"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (menu_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Main Menu"), -1);
  adw_header_bar_pack_end (ADW_HEADER_BAR (sidebar_header), menu_button);
  const char *connections[] = { _ ("Session"), _ ("System"), NULL };
  self->connections = gtk_drop_down_new_from_strings (connections);
  g_autoptr (GtkListItemFactory) connection_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (connection_factory, "setup", G_CALLBACK (connection_setup), NULL);
  g_signal_connect (connection_factory, "bind", G_CALLBACK (connection_bind), NULL);
  gtk_drop_down_set_factory (GTK_DROP_DOWN (self->connections), connection_factory);
  gtk_widget_set_tooltip_text (self->connections, _ ("Choose the local libvirt connection"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->connections), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Choose the local libvirt connection"), -1);
  g_signal_connect (self->connections, "notify::selected", G_CALLBACK (connection_changed), self);
  GtkWidget *connection_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top (connection_row, 6);
  gtk_widget_set_margin_bottom (connection_row, 6);
  gtk_widget_set_margin_start (connection_row, 6);
  gtk_widget_set_margin_end (connection_row, 6);
  GtkWidget *connection_label = gtk_label_new (_ ("Connection"));
  gtk_label_set_xalign (GTK_LABEL (connection_label), 0);
  gtk_widget_set_hexpand (self->connections, TRUE);
  gtk_box_append (GTK_BOX (connection_row), connection_label);
  gtk_box_append (GTK_BOX (connection_row), self->connections);
  gtk_popover_menu_add_child (
      GTK_POPOVER_MENU (gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu_button))),
      connection_row, "connection");
  GtkWidget *search_button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (search_button), "system-search-symbolic");
  gtk_widget_set_tooltip_text (search_button, _ ("Search Virtual Machines"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (search_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Search Virtual Machines"), -1);
  g_object_bind_property (g_action_map_lookup_action (G_ACTION_MAP (self), "search"), "enabled",
                          search_button, "sensitive", G_BINDING_SYNC_CREATE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (sidebar_header), search_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (sidebar_toolbar), sidebar_header);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (content_toolbar), header);
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (paned, "pirwa-machines");
  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  self->stack = GTK_STACK (gtk_stack_new ());
  adw_toast_overlay_set_child (self->toasts, paned);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->toasts));
  GtkWidget *loading = adw_status_page_new ();
  self->loading_spinner = GTK_SPINNER (gtk_spinner_new ());
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->loading_spinner),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _ ("Loading virtual machines"),
                                  -1);
  adw_status_page_set_child (ADW_STATUS_PAGE (loading), GTK_WIDGET (self->loading_spinner));
  gtk_stack_add_named (self->stack, loading, "loading");
  GtkWidget *empty = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), _ ("No Virtual Machines"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (empty),
      _ ("Create a Linux virtual machine or import a standalone qcow2 image."));
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "computer-symbolic");
  adw_status_page_set_child (ADW_STATUS_PAGE (empty),
                             action_button (_ ("New Virtual Machine"), "win.new"));
  gtk_stack_add_named (self->stack, empty, "empty");
  self->error_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_title (self->error_page, _ ("Connection Unavailable"));
  adw_status_page_set_icon_name (self->error_page, "network-error-symbolic");
  adw_status_page_set_child (self->error_page, action_button (_ ("Reconnect"), "win.refresh"));
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->error_page), "error");
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->search_bar = GTK_SEARCH_BAR (gtk_search_bar_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->search_bar), "pirwa-search");
  gtk_widget_add_css_class (GTK_WIDGET (self->search_bar), "inline");
  self->search = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->search), TRUE);
  gtk_widget_set_margin_top (GTK_WIDGET (self->search), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->search), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self->search), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (self->search), 6);
  gtk_search_bar_set_child (self->search_bar, GTK_WIDGET (self->search));
  gtk_search_bar_connect_entry (self->search_bar, GTK_EDITABLE (self->search));
  g_object_bind_property (search_button, "active", self->search_bar, "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  gtk_box_append (GTK_BOX (left), GTK_WIDGET (self->search_bar));
  g_signal_connect (self->search, "search-changed", G_CALLBACK (search_changed), self);
  g_signal_connect_object (self->search_bar, "notify::search-mode-enabled",
                           G_CALLBACK (search_mode_changed), self, 0);
  self->filter = gtk_custom_filter_new (matches_search, self, NULL);
  GtkFilterListModel *filtered = gtk_filter_list_model_new (
      G_LIST_MODEL (g_object_ref (self->machines)), GTK_FILTER (self->filter));
  self->selection = gtk_single_selection_new (G_LIST_MODEL (filtered));
  gtk_single_selection_set_autoselect (self->selection, FALSE);
  gtk_single_selection_set_can_unselect (self->selection, TRUE);
  g_signal_connect (self->selection, "notify::selected-item", G_CALLBACK (selection_changed), self);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (factory_setup), self);
  g_signal_connect (factory, "bind", G_CALLBACK (factory_bind), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (factory_unbind), NULL);
  GtkWidget *list
      = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (self->selection)), factory);
  self->list = list;
  gtk_widget_add_css_class (list, "navigation-sidebar");
  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), list);
  gtk_widget_set_vexpand (list_scroll, TRUE);
  gtk_box_append (GTK_BOX (left), list_scroll);
  gtk_widget_set_size_request (left, 260, -1);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (sidebar_toolbar), left);
  gtk_paned_set_start_child (GTK_PANED (paned), sidebar_toolbar);
  gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_position (GTK_PANED (paned), 300);
  self->viewer_stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->viewer_stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->viewer_stack), TRUE);
  self->viewer_host = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_stack_add_named (self->viewer_stack, GTK_WIDGET (self->viewer_host), "viewer");

  self->viewer_status = ADW_STATUS_PAGE (adw_status_page_new ());
  GtkWidget *information = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  self->avatar = pirwa_avatar_new (NULL, 96);
  gtk_box_append (GTK_BOX (information), self->avatar);
  self->name = GTK_LABEL (gtk_label_new (_ ("Select a Virtual Machine")));
  gtk_widget_add_css_class (GTK_WIDGET (self->name), "title-1");
  gtk_label_set_wrap (self->name, TRUE);
  gtk_label_set_wrap_mode (self->name, PANGO_WRAP_WORD_CHAR);
  self->start_progress = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_widget_set_halign (GTK_WIDGET (self->start_progress), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (GTK_WIDGET (self->start_progress), 150, -1);
  gtk_widget_set_visible (GTK_WIDGET (self->start_progress), FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->start_progress),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _ ("Starting virtual machine"),
                                  -1);
  self->state = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->state, TRUE);
  self->resources = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->resources, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->resources), "dim-label");
  gtk_box_append (GTK_BOX (information), GTK_WIDGET (self->name));
  gtk_box_append (GTK_BOX (information), GTK_WIDGET (self->start_progress));
  gtk_box_append (GTK_BOX (information), GTK_WIDGET (self->state));
  gtk_box_append (GTK_BOX (information), GTK_WIDGET (self->resources));
  self->start_button = gtk_button_new ();
  gtk_actionable_set_action_name (GTK_ACTIONABLE (self->start_button), "win.start");
  GtkWidget *start_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (start_content),
                  gtk_image_new_from_icon_name ("system-shutdown-symbolic"));
  gtk_box_append (GTK_BOX (start_content), gtk_label_new (_ ("Start")));
  gtk_button_set_child (GTK_BUTTON (self->start_button), start_content);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->start_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _ ("Start"), -1);
  gtk_widget_add_css_class (self->start_button, "text-button");
  gtk_widget_add_css_class (self->start_button, "suggested-action");
  gtk_widget_add_css_class (self->start_button, "pill");
  gtk_widget_set_halign (self->start_button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (self->start_button, 6);
  gtk_box_append (GTK_BOX (information), self->start_button);
  adw_status_page_set_child (self->viewer_status, information);
  gtk_stack_add_named (self->viewer_stack, GTK_WIDGET (self->viewer_status), "status");

  GtkWidget *detached = gtk_overlay_new ();
  self->preview = GTK_PICTURE (gtk_picture_new ());
  gtk_widget_set_margin_start (GTK_WIDGET (self->preview), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (self->preview), 6);
  gtk_widget_set_margin_top (GTK_WIDGET (self->preview), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->preview), 6);
  gtk_picture_set_content_fit (self->preview, GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_opacity (GTK_WIDGET (self->preview), 0.45);
  gtk_overlay_set_child (GTK_OVERLAY (detached), GTK_WIDGET (self->preview));
  GtkWidget *detached_content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_halign (detached_content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (detached_content, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (detached_content, 18);
  gtk_widget_set_margin_end (detached_content, 18);
  GtkWidget *detached_label = gtk_label_new (_ ("Viewer is in a separate window"));
  gtk_widget_add_css_class (detached_label, "title-2");
  gtk_label_set_wrap (GTK_LABEL (detached_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (detached_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (detached_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (detached_content), detached_label);
  GtkWidget *bring_back = action_button (_ ("Bring Viewer Back"), "win.return-viewer");
  gtk_widget_add_css_class (bring_back, "suggested-action");
  gtk_widget_add_css_class (bring_back, "pill");
  gtk_widget_set_halign (bring_back, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (bring_back, 12);
  gtk_widget_set_margin_end (bring_back, 12);
  gtk_box_append (GTK_BOX (detached_content), bring_back);
  gtk_overlay_add_overlay (GTK_OVERLAY (detached), detached_content);
  gtk_stack_add_named (self->viewer_stack, detached, "detached");
  gtk_stack_set_visible_child_name (self->viewer_stack, "status");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->viewer_stack), "machines");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (content_toolbar), GTK_WIDGET (self->stack));
  gtk_paned_set_end_child (GTK_PANED (paned), content_toolbar);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  self->operation_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->stage = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->stage, TRUE);
  gtk_label_set_wrap_mode (self->stage, PANGO_WRAP_WORD_CHAR);
  self->progress = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->progress), TRUE);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Cancel"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (cancel_operation), self);
  gtk_box_append (GTK_BOX (self->operation_bar), GTK_WIDGET (self->stage));
  gtk_box_append (GTK_BOX (self->operation_bar), GTK_WIDGET (self->progress));
  gtk_box_append (GTK_BOX (self->operation_bar), cancel);
  gtk_widget_set_visible (self->operation_bar, FALSE);
  gtk_widget_set_margin_top (self->operation_bar, 6);
  gtk_widget_set_margin_bottom (self->operation_bar, 6);
  gtk_widget_set_margin_start (self->operation_bar, 12);
  gtk_widget_set_margin_end (self->operation_bar, 12);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (content_toolbar), self->operation_bar);
  /* Fit readings to their text; allow zero minimum width to keep controls usable. */
  self->header_resources = g_object_new (GTK_TYPE_LABEL, "ellipsize", PANGO_ELLIPSIZE_END, "valign",
                                         GTK_ALIGN_CENTER, NULL);
  gtk_widget_add_css_class (GTK_WIDGET (self->header_resources), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->header_resources), "caption");
  gtk_label_set_xalign (self->header_resources, 0);
  g_object_bind_property (self->resources, "label", self->header_resources, "label",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->header_resources, "label", self->header_resources, "tooltip-text",
                          G_BINDING_SYNC_CREATE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->header_resources),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _ ("Selected VM’s configured CPU count and memory"), -1);
  self->usage = g_object_new (GTK_TYPE_LABEL, "label", _ ("Sampling usage…"), "ellipsize",
                              PANGO_ELLIPSIZE_END, "valign", GTK_ALIGN_CENTER, NULL);
  gtk_widget_add_css_class (GTK_WIDGET (self->usage), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->usage), "caption");
  gtk_label_set_xalign (self->usage, 1);
  gtk_widget_set_visible (GTK_WIDGET (self->usage), FALSE);
  g_object_bind_property (self->usage, "label", self->usage, "tooltip-text", G_BINDING_SYNC_CREATE);
  gtk_accessible_update_property (
      GTK_ACCESSIBLE (self->usage), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
      _ ("Selected VM usage. CPU 100% equals one host CPU; multiple CPUs can exceed 100%. "
         "RAM is the VM process’s resident memory (RSS), including overhead. "
         "Updates every two seconds while this window is visible."),
      -1);
  self->network = g_object_new (GTK_TYPE_LABEL, "label", _ ("NET …"), "ellipsize",
                                PANGO_ELLIPSIZE_END, "valign", GTK_ALIGN_CENTER, NULL);
  gtk_widget_add_css_class (GTK_WIDGET (self->network), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->network), "caption");
  gtk_label_set_xalign (self->network, 1);
  gtk_widget_set_visible (GTK_WIDGET (self->network), FALSE);
  g_object_bind_property (self->network, "label", self->network, "tooltip-text",
                          G_BINDING_SYNC_CREATE);
  gtk_accessible_update_property (
      GTK_ACCESSIBLE (self->network), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
      _ ("Selected VM network usage. Down arrow: received bytes per second. "
         "Up arrow: transmitted bytes per second. Rates include all VM interfaces. "
         "Updates every two seconds while this window is visible."),
      -1);
  GtkWidget *readings[] = { GTK_WIDGET (self->header_resources), GTK_WIDGET (self->usage),
                            GTK_WIDGET (self->network) };
  for (guint i = 0; i < G_N_ELEMENTS (readings); i++)
    {
      gtk_widget_set_layout_manager (
          readings[i], gtk_custom_layout_new (NULL, measure_header_reading,
                                              GTK_WIDGET_GET_CLASS (readings[i])->size_allocate));
      gtk_widget_set_overflow (readings[i], GTK_OVERFLOW_HIDDEN);
    }
  gtk_box_append (GTK_BOX (header_info), GTK_WIDGET (self->header_resources));
  gtk_box_append (GTK_BOX (header_info), GTK_WIDGET (self->usage));
  gtk_box_append (GTK_BOX (header_info), GTK_WIDGET (self->network));
  gtk_box_append (GTK_BOX (header_info), GTK_WIDGET (self->header_state));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toolbar);
  gtk_window_set_title (GTK_WINDOW (self), "Pirwa");
  gtk_window_set_default_size (GTK_WINDOW (self), 1040, 680);
  g_signal_connect (self, "close-request", G_CALLBACK (window_close_requested), NULL);
  g_signal_connect (self, "map", G_CALLBACK (window_mapped), NULL);
  g_signal_connect (self, "unmap", G_CALLBACK (window_unmapped), NULL);
  g_signal_connect (self, "notify::suspended", G_CALLBACK (usage_visibility_changed), NULL);
  g_signal_connect (self, "notify::visible-dialog", G_CALLBACK (dialog_changed), NULL);
  g_signal_connect_object (self->stack, "notify::visible-child", G_CALLBACK (loading_page_changed),
                           self, 0);
  loading_page_changed (G_OBJECT (self->stack), NULL, self);
  update_actions (self);
}

GtkWindow *
pirwa_window_new (GtkApplication *application, const char *uri)
{
  return pirwa_window_new_with_boxes (application, uri, NULL);
}

GtkWindow *
pirwa_window_new_with_boxes (GtkApplication *application, const char *uri,
                             const PirwaBoxesPaths *paths)
{
  PirwaWindow *self = g_object_new (PIRWA_TYPE_WINDOW, "application", application, NULL);
  self->boxes_paths = paths
                          ? pirwa_boxes_paths_copy (paths)
                          : pirwa_boxes_paths_new (g_str_has_prefix (uri, "test:///") ? uri : NULL);
  self->updating = TRUE;
  if (g_str_has_prefix (uri, "test:///"))
    gtk_string_list_append (
        GTK_STRING_LIST (gtk_drop_down_get_model (GTK_DROP_DOWN (self->connections))),
        _ ("Test Driver"));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->connections),
                              g_str_has_prefix (uri, "test:///")    ? 2
                              : g_str_equal (uri, "qemu:///system") ? 1
                                                                    : 0);
  self->updating = FALSE;
  open_connection (self, uri);
  if (paths || !g_str_has_prefix (uri, "test:///"))
    start_boxes_scan (self, TRUE);
  const char *new_accels[] = { "<Primary>n", NULL };
  const char *refresh_accels[] = { "<Primary>r", "F5", NULL };
  const char *search_accels[] = { "<Primary>f", NULL };
  gtk_application_set_accels_for_action (application, "win.new", new_accels);
  gtk_application_set_accels_for_action (application, "win.refresh", refresh_accels);
  gtk_application_set_accels_for_action (application, "win.search", search_accels);
  return GTK_WINDOW (self);
}
