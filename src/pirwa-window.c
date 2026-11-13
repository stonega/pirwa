/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-window.h"
#include "config.h"
#include "pirwa-backend.h"
#include <glib/gi18n.h>

struct _PirwaWindow
{
  AdwApplicationWindow parent_instance;
  PirwaBackend *backend;
  GCancellable *lifetime;
  GCancellable *operation_cancel;
  GCancellable *detail_cancel;
  GListStore *machines;
  GtkSingleSelection *selection;
  GtkCustomFilter *filter;
  GtkStack *stack;
  AdwToastOverlay *toasts;
  AdwStatusPage *error_page;
  GtkLabel *name;
  GtkLabel *state;
  GtkLabel *resources;
  GtkLabel *status;
  GtkLabel *stage;
  GtkProgressBar *progress;
  GtkWidget *operation_bar;
  GtkWidget *connections;
  GtkSearchEntry *search;
  GStrv pools;
  char *search_text;
  PirwaDetails *details;
  gboolean busy;
  gboolean updating;
  gboolean connected;
};

G_DEFINE_FINAL_TYPE (PirwaWindow, pirwa_window, ADW_TYPE_APPLICATION_WINDOW)

static void show_form (PirwaWindow *self, gboolean edit);
static void submit (PirwaWindow *self, const PirwaRequest *request, AdwDialog *dialog);
static void load_details (PirwaWindow *self);

static PirwaMachine *
selected (PirwaWindow *self)
{
  return self->selection ? gtk_single_selection_get_selected_item (self->selection) : NULL;
}

static void
toast (PirwaWindow *self, const char *message)
{
  if (self->lifetime && !g_cancellable_is_cancelled (self->lifetime))
    adw_toast_overlay_add_toast (self->toasts, adw_toast_new (message));
}

static void
set_action (PirwaWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
update_actions (PirwaWindow *self)
{
  PirwaMachine *machine = selected (self);
  int state = machine ? pirwa_machine_get_state_code (machine) : 0;
  gboolean ready = machine && !self->busy;
  gboolean stopped = state == 5 || state == 6;
  set_action (self, "start", ready && stopped);
  set_action (self, "shutdown", ready && state == 1);
  set_action (self, "reboot", ready && state == 1);
  set_action (self, "pause", ready && state == 1);
  set_action (self, "resume", ready && state == 3);
  set_action (self, "force-stop", ready && state != 5 && state != 6 && state != 0);
  set_action (self, "console", ready && (state == 1 || state == 3));
  set_action (self, "edit", ready && stopped && self->details != NULL);
  set_action (self, "remove", ready && stopped && self->details != NULL);
  set_action (self, "new", !self->busy && self->connected);
  set_action (self, "refresh", !self->busy);
  gtk_widget_set_sensitive (self->connections, !self->busy);
  if (machine)
    {
      gtk_label_set_text (self->name, pirwa_machine_get_name (machine));
      gtk_label_set_text (self->state, pirwa_machine_get_state (machine));
    }
  else
    {
      gtk_label_set_text (self->name, _ ("Select a Virtual Machine"));
      gtk_label_set_text (self->state, _ ("Choose a machine to view its controls."));
      gtk_label_set_text (self->resources, "");
    }
}

static gboolean
matches_search (gpointer item, gpointer user_data)
{
  PirwaWindow *self = user_data;
  if (!self->search_text || !*self->search_text)
    return TRUE;
  g_autofree char *name = g_utf8_casefold (pirwa_machine_get_name (item), -1);
  return strstr (name, self->search_text) != NULL;
}

static void
search_changed (GtkSearchEntry *entry, PirwaWindow *self)
{
  g_free (self->search_text);
  self->search_text = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
row_changed (PirwaMachine *machine, GtkWidget *row)
{
  gtk_label_set_text (g_object_get_data (G_OBJECT (row), "name"), pirwa_machine_get_name (machine));
  gtk_label_set_text (g_object_get_data (G_OBJECT (row), "state"),
                      pirwa_machine_get_state (machine));
}

static void
factory_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
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
  gtk_box_append (GTK_BOX (row), gtk_image_new_from_icon_name ("computer-symbolic"));
  gtk_box_append (GTK_BOX (labels), name);
  gtk_box_append (GTK_BOX (labels), state);
  gtk_box_append (GTK_BOX (row), labels);
  g_object_set_data (G_OBJECT (row), "name", name);
  g_object_set_data (G_OBJECT (row), "state", state);
  gtk_list_item_set_child (item, row);
}

static void
factory_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  PirwaMachine *machine = gtk_list_item_get_item (item);
  GtkWidget *row = gtk_list_item_get_child (item);
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
  if (machine)
    g_signal_handlers_disconnect_by_data (machine, gtk_list_item_get_child (item));
  g_object_set_data (G_OBJECT (item), "machine", NULL);
}

static void
apply_inventory (PirwaWindow *self, PirwaReply *reply)
{
  if (!reply->machines)
    return;
  self->connected = TRUE;
  PirwaMachine *current = selected (self);
  g_autofree char *selected_uuid = current ? g_strdup (pirwa_machine_get_uuid (current)) : NULL;
  g_autoptr (GHashTable) existing
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  g_autoptr (GPtrArray) ordered = g_ptr_array_new_with_free_func (g_object_unref);
  guint old_count = g_list_model_get_n_items (G_LIST_MODEL (self->machines));
  for (guint i = 0; i < old_count; i++)
    {
      PirwaMachine *machine = g_list_model_get_item (G_LIST_MODEL (self->machines), i);
      g_hash_table_insert (existing, g_strdup (pirwa_machine_get_uuid (machine)), machine);
    }
  for (guint i = 0; i < reply->machines->len; i++)
    {
      PirwaMachine *snapshot = g_ptr_array_index (reply->machines, i);
      PirwaMachine *machine = g_hash_table_lookup (existing, pirwa_machine_get_uuid (snapshot));
      if (machine)
        pirwa_machine_update (machine, snapshot);
      g_ptr_array_add (ordered, g_object_ref (machine ? machine : snapshot));
    }
  guint prefix = 0, suffix = 0;
  while (prefix < old_count && prefix < ordered->len)
    {
      g_autoptr (PirwaMachine) old = g_list_model_get_item (G_LIST_MODEL (self->machines), prefix);
      if (old != g_ptr_array_index (ordered, prefix))
        break;
      prefix++;
    }
  while (suffix < old_count - prefix && suffix < ordered->len - prefix)
    {
      g_autoptr (PirwaMachine) old
          = g_list_model_get_item (G_LIST_MODEL (self->machines), old_count - suffix - 1);
      if (old != g_ptr_array_index (ordered, ordered->len - suffix - 1))
        break;
      suffix++;
    }
  self->updating = TRUE;
  if (old_count - prefix - suffix || ordered->len - prefix - suffix)
    g_list_store_splice (self->machines, prefix, old_count - prefix - suffix,
                         ordered->pdata ? ordered->pdata + prefix : NULL,
                         ordered->len - prefix - suffix);
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
  if (selected_uuid)
    {
      guint position = GTK_INVALID_LIST_POSITION;
      GListModel *model = gtk_single_selection_get_model (self->selection);
      for (guint i = 0; i < g_list_model_get_n_items (model); i++)
        {
          g_autoptr (PirwaMachine) machine = g_list_model_get_item (model, i);
          if (g_str_equal (selected_uuid, pirwa_machine_get_uuid (machine)))
            {
              position = i;
              break;
            }
        }
      gtk_single_selection_set_selected (self->selection, position);
    }
  self->updating = FALSE;
  if (reply->pools)
    {
      g_strfreev (self->pools);
      self->pools = g_strdupv (reply->pools);
    }
  if (reply->host)
    gtk_label_set_text (self->status, reply->host);
  gtk_stack_set_visible_child_name (self->stack, ordered->len ? "machines" : "empty");
  if (!selected (self))
    g_clear_pointer (&self->details, pirwa_details_free);
  update_actions (self);
}

static void
backend_inventory (PirwaBackend *backend, PirwaReply *reply, PirwaWindow *self)
{
  if (backend == self->backend && self->lifetime)
    apply_inventory (self, reply);
}

static void
backend_error (PirwaBackend *backend, const char *message, PirwaWindow *self)
{
  if (backend != self->backend || !self->lifetime)
    return;
  self->connected = FALSE;
  g_autofree char *escaped = g_markup_escape_text (message, -1);
  adw_status_page_set_description (self->error_page, escaped);
  gtk_stack_set_visible_child_name (self->stack, "error");
  update_actions (self);
}

static void
backend_progress (PirwaBackend *backend, double fraction, const char *stage, PirwaWindow *self)
{
  if (backend != self->backend || !self->busy)
    return;
  gtk_progress_bar_set_fraction (self->progress, fraction);
  gtk_label_set_text (self->stage, stage);
}

typedef struct
{
  PirwaWindow *window;
  AdwDialog *dialog;
  PirwaOperation operation;
  char *uuid;
  GApplication *application;
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
request_done (GObject *source, GAsyncResult *result, gpointer data)
{
  RequestContext *context = data;
  PirwaWindow *self = context->window;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &error);
  if (!self->lifetime || source != G_OBJECT (self->backend))
    {
      context_free (context);
      return;
    }
  if (context->operation == PIRWA_DETAILS)
    {
      PirwaMachine *machine = selected (self);
      if (reply && machine && g_strcmp0 (context->uuid, pirwa_machine_get_uuid (machine)) == 0)
        {
          g_clear_pointer (&self->details, pirwa_details_free);
          self->details = g_steal_pointer (&reply->details);
          g_autofree char *text = g_strdup_printf (_ ("%u CPUs · %u MiB RAM"), self->details->cpus,
                                                   self->details->memory_mb);
          gtk_label_set_text (self->resources, text);
          update_actions (self);
        }
      else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        toast (self, error->message);
      context_free (context);
      return;
    }
  self->busy = FALSE;
  gtk_widget_set_visible (self->operation_bar, FALSE);
  if (context->dialog)
    adw_dialog_set_can_close (context->dialog, TRUE);
  if (!reply)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          toast (self, error->message);
          if (context->operation <= PIRWA_RECONNECT)
            backend_error (self->backend, error->message, self);
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
      if (context->dialog)
        adw_dialog_close (context->dialog);
      if (context->operation == PIRWA_SHUTDOWN)
        toast (self, _ ("Shutdown requested. Waiting for the guest."));
      if (context->operation == PIRWA_REBOOT)
        toast (self, _ ("Restart requested. Waiting for the guest."));
      apply_inventory (self, reply);
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
          toast (self, _ ("Virtual machine created and started. Choose Open Console to continue."));
        }
      if (context->operation != PIRWA_CONSOLE)
        load_details (self);
    }
  update_actions (self);
  context_free (context);
}

static void
submit (PirwaWindow *self, const PirwaRequest *request, AdwDialog *dialog)
{
  if (self->busy || !self->lifetime)
    return;
  self->busy = TRUE;
  g_clear_object (&self->operation_cancel);
  self->operation_cancel = g_cancellable_new ();
  gtk_widget_set_visible (self->operation_bar, TRUE);
  gtk_progress_bar_set_fraction (self->progress, 0);
  gtk_label_set_text (self->stage, _ ("Working…"));
  if (dialog)
    {
      adw_dialog_set_can_close (dialog, FALSE);
      GtkWidget *apply = g_object_get_data (G_OBJECT (dialog), "apply");
      if (apply)
        gtk_widget_set_sensitive (apply, FALSE);
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
  pirwa_backend_request_async (self->backend, request, self->operation_cancel, request_done,
                               context);
}

static void
load_details (PirwaWindow *self)
{
  if (self->detail_cancel)
    g_cancellable_cancel (self->detail_cancel);
  g_clear_object (&self->detail_cancel);
  g_clear_pointer (&self->details, pirwa_details_free);
  PirwaMachine *machine = selected (self);
  if (!machine || !self->lifetime)
    {
      update_actions (self);
      return;
    }
  self->detail_cancel = g_cancellable_new ();
  RequestContext *context = g_new0 (RequestContext, 1);
  context->window = g_object_ref (self);
  context->operation = PIRWA_DETAILS;
  context->uuid = g_strdup (pirwa_machine_get_uuid (machine));
  PirwaRequest request = { .operation = PIRWA_DETAILS, .uuid = context->uuid };
  gtk_label_set_text (self->resources, _ ("Loading resources…"));
  pirwa_backend_request_async (self->backend, &request, self->detail_cancel, request_done, context);
  update_actions (self);
}

static void
selection_changed (GObject *object, GParamSpec *spec, PirwaWindow *self)
{
  (void)object;
  (void)spec;
  if (!self->updating)
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
action_activated (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)parameter;
  PirwaWindow *self = data;
  const char *name = g_action_get_name (G_ACTION (action));
  if (g_str_equal (name, "new"))
    {
      show_form (self, FALSE);
      return;
    }
  if (g_str_equal (name, "edit"))
    {
      show_form (self, TRUE);
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
  const char *names[] = { "refresh", "start", "shutdown", "reboot", "pause", "resume", "console" };
  const PirwaOperation operations[] = { PIRWA_RECONNECT, PIRWA_START,  PIRWA_SHUTDOWN, PIRWA_REBOOT,
                                        PIRWA_PAUSE,     PIRWA_RESUME, PIRWA_CONSOLE };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_str_equal (name, names[i]))
      {
        PirwaMachine *machine = selected (self);
        PirwaRequest request = { .operation = operations[i],
                                 .uuid = machine ? pirwa_machine_get_uuid (machine) : NULL };
        submit (self, &request, NULL);
        return;
      }
}

typedef struct
{
  PirwaWindow *window;
  AdwDialog *dialog;
  AdwEntryRow *name;
  AdwEntryRow *source;
  AdwComboRow *pool;
  AdwSpinRow *cpus;
  AdwSpinRow *memory;
  AdwSpinRow *disk;
  GtkCheckButton *import;
  GtkCheckButton *eject;
  GtkCheckButton *bios;
  GtkLabel *error;
  char *uuid;
  GStrv pools;
} Form;

static void
form_free (gpointer data)
{
  Form *form = data;
  g_object_unref (form->window);
  g_free (form->uuid);
  g_strfreev (form->pools);
  g_free (form);
}

static void
file_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (AdwDialog) dialog = data;
  Form *form = g_object_get_data (G_OBJECT (dialog), "form");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
  if (file)
    {
      g_autofree char *path = g_file_get_path (file);
      if (path)
        gtk_editable_set_text (GTK_EDITABLE (form->source), path);
    }
  else if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
           && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    gtk_label_set_text (form->error, error->message);
}

static void
choose_source (GtkButton *button, Form *form)
{
  (void)button;
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _ ("Select ISO or qcow2 Image"));
  gtk_file_dialog_open (dialog, GTK_WINDOW (form->window), form->window->lifetime, file_chosen,
                        g_object_ref (form->dialog));
}

static void
form_apply (GtkButton *button, Form *form)
{
  (void)button;
  PirwaRequest request
      = { .operation = form->uuid ? PIRWA_EDIT : PIRWA_CREATE, .uuid = form->uuid };
  request.options.name = (char *)gtk_editable_get_text (GTK_EDITABLE (form->name));
  request.options.cpus = adw_spin_row_get_value (form->cpus);
  request.options.memory_mb = adw_spin_row_get_value (form->memory);
  request.options.disk_gb = form->disk ? adw_spin_row_get_value (form->disk) : 4;
  request.eject = form->eject && gtk_check_button_get_active (form->eject);
  if (form->source)
    {
      request.options.source = (char *)gtk_editable_get_text (GTK_EDITABLE (form->source));
      request.options.import_image = gtk_check_button_get_active (form->import);
      request.options.legacy_bios = gtk_check_button_get_active (form->bios);
      guint index = adw_combo_row_get_selected (form->pool);
      request.options.pool = form->pools[index];
    }
  g_autoptr (GError) error = NULL;
  if (!pirwa_config_validate (&request.options, &error))
    {
      gtk_label_set_text (form->error, error->message);
      gtk_widget_set_visible (GTK_WIDGET (form->error), TRUE);
      return;
    }
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
show_form (PirwaWindow *self, gboolean edit)
{
  if (self->busy || (edit && !self->details))
    return;
  Form *form = g_new0 (Form, 1);
  form->window = g_object_ref (self);
  form->dialog = adw_dialog_new ();
  adw_dialog_set_title (form->dialog,
                        edit ? _ ("Virtual Machine Settings") : _ ("New Virtual Machine"));
  adw_dialog_set_content_width (form->dialog, 540);
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
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _ ("Resources"));
  gtk_box_append (GTK_BOX (box), group);
  form->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (form->name), _ ("Name"));
  gtk_editable_set_text (GTK_EDITABLE (form->name),
                         edit ? self->details->name : _ ("New Virtual Machine"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (form->name));
  form->cpus = spin_row (group, _ ("Virtual CPUs"), edit ? self->details->cpus : 2, 1, 64, 1);
  form->memory = spin_row (group, _ ("Memory (MiB)"), edit ? self->details->memory_mb : 2048, 256,
                           262144, 256);
  if (edit)
    {
      form->uuid = g_strdup (pirwa_machine_get_uuid (selected (self)));
      if (self->details->has_cdrom)
        {
          form->eject
              = GTK_CHECK_BUTTON (gtk_check_button_new_with_label (_ ("Eject installation media")));
          gtk_box_append (GTK_BOX (box), GTK_WIDGET (form->eject));
        }
    }
  else
    {
      form->disk = spin_row (group, _ ("Disk capacity (GiB)"), 32, 4, 2048, 1);
      GtkWidget *media_group = adw_preferences_group_new ();
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (media_group),
                                       _ ("Installation Media"));
      gtk_box_append (GTK_BOX (box), media_group);
      form->source = ADW_ENTRY_ROW (adw_entry_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (form->source),
                                     _ ("Local ISO or qcow2 path"));
      GtkWidget *browse = gtk_button_new_from_icon_name ("document-open-symbolic");
      gtk_widget_set_valign (browse, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (browse, _ ("Choose a file"));
      adw_entry_row_add_suffix (form->source, browse);
      g_signal_connect (browse, "clicked", G_CALLBACK (choose_source), form);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group), GTK_WIDGET (form->source));
      form->import = GTK_CHECK_BUTTON (
          gtk_check_button_new_with_label (_ ("Import a standalone qcow2 image")));
      gtk_box_append (GTK_BOX (box), GTK_WIDGET (form->import));
      form->bios = GTK_CHECK_BUTTON (
          gtk_check_button_new_with_label (_ ("Use legacy BIOS instead of UEFI")));
      gtk_box_append (GTK_BOX (box), GTK_WIDGET (form->bios));
      GtkWidget *hint
          = gtk_label_new (_ ("Import copies the source into a new owned volume and uses its "
                              "virtual capacity. The original image stays unchanged."));
      gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
      gtk_widget_add_css_class (hint, "dim-label");
      gtk_box_append (GTK_BOX (box), hint);
      GPtrArray *pools = g_ptr_array_new_with_free_func (g_free);
      if (g_str_equal (pirwa_backend_get_uri (self->backend), "qemu:///session"))
        g_ptr_array_add (pools, g_strdup ("pirwa"));
      for (guint i = 0; self->pools && self->pools[i]; i++)
        if (!g_str_equal (self->pools[i], "pirwa") || pools->len == 0)
          g_ptr_array_add (pools, g_strdup (self->pools[i]));
      if (pools->len == 0)
        g_ptr_array_add (pools, g_strdup (""));
      g_ptr_array_add (pools, NULL);
      form->pools = (GStrv)g_ptr_array_free (pools, FALSE);
      form->pool = ADW_COMBO_ROW (adw_combo_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (form->pool),
                                     _ ("Directory storage pool"));
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (form->pool), FALSE);
      GtkStringList *model = gtk_string_list_new ((const char *const *)form->pools);
      adw_combo_row_set_model (form->pool, G_LIST_MODEL (model));
      g_object_unref (model);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group), GTK_WIDGET (form->pool));
    }
  form->error = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (form->error, TRUE);
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
  GtkWidget *apply = gtk_button_new_with_label (edit ? _ ("Save Changes") : _ ("Create and Start"));
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
open_connection (PirwaWindow *self, const char *uri)
{
  self->connected = FALSE;
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
  PirwaRequest request = { .operation = PIRWA_REFRESH };
  submit (self, &request, NULL);
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

static gboolean
window_close_requested (GtkWindow *window, gpointer data)
{
  (void)data;
  PirwaWindow *self = PIRWA_WINDOW (window);
  if (self->operation_cancel)
    g_cancellable_cancel (self->operation_cancel);
  if (self->detail_cancel)
    g_cancellable_cancel (self->detail_cancel);
  if (self->lifetime)
    g_cancellable_cancel (self->lifetime);
  if (self->backend)
    pirwa_backend_stop (self->backend);
  return FALSE;
}

static void
pirwa_window_dispose (GObject *object)
{
  PirwaWindow *self = PIRWA_WINDOW (object);
  window_close_requested (GTK_WINDOW (self), NULL);
  g_clear_object (&self->backend);
  g_clear_object (&self->lifetime);
  g_clear_object (&self->operation_cancel);
  g_clear_object (&self->detail_cancel);
  g_clear_object (&self->selection);
  g_clear_object (&self->machines);
  G_OBJECT_CLASS (pirwa_window_parent_class)->dispose (object);
}

static void
pirwa_window_finalize (GObject *object)
{
  PirwaWindow *self = PIRWA_WINDOW (object);
  g_free (self->search_text);
  g_strfreev (self->pools);
  pirwa_details_free (self->details);
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
pirwa_window_init (PirwaWindow *self)
{
  self->lifetime = g_cancellable_new ();
  self->machines = g_list_store_new (PIRWA_TYPE_MACHINE);
  const char *actions[] = { "new",    "refresh",    "start",   "shutdown", "reboot", "pause",
                            "resume", "force-stop", "console", "edit",     "remove" };
  for (guint i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      GSimpleAction *action = g_simple_action_new (actions[i], NULL);
      g_signal_connect (action, "activate", G_CALLBACK (action_activated), self);
      g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
      g_object_unref (action);
    }
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *title = adw_window_title_new ("Pirwa", _ ("Virtual Machines"));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), title);
  GtkWidget *new_button = action_button (_ ("New Virtual Machine"), "win.new");
  gtk_widget_add_css_class (new_button, "suggested-action");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_button);
  const char *connections[] = { _ ("Session"), _ ("System"), NULL };
  self->connections = gtk_drop_down_new_from_strings (connections);
  gtk_widget_set_tooltip_text (self->connections, _ ("Choose the local libvirt connection"));
  g_signal_connect (self->connections, "notify::selected", G_CALLBACK (connection_changed), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->connections);
  GtkWidget *refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (refresh), "win.refresh");
  gtk_widget_set_tooltip_text (refresh, _ ("Refresh and reconnect"));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), refresh);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  self->stack = GTK_STACK (gtk_stack_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (self->stack));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->toasts));
  GtkWidget *loading = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (loading), _ ("Connecting to libvirt"));
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (spinner));
  adw_status_page_set_child (ADW_STATUS_PAGE (loading), spinner);
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
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->search = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_margin_top (GTK_WIDGET (self->search), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->search), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self->search), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->search), 12);
  gtk_box_append (GTK_BOX (left), GTK_WIDGET (self->search));
  g_signal_connect (self->search, "search-changed", G_CALLBACK (search_changed), self);
  self->filter = gtk_custom_filter_new (matches_search, self, NULL);
  GtkFilterListModel *filtered = gtk_filter_list_model_new (
      G_LIST_MODEL (g_object_ref (self->machines)), GTK_FILTER (self->filter));
  self->selection = gtk_single_selection_new (G_LIST_MODEL (filtered));
  gtk_single_selection_set_autoselect (self->selection, FALSE);
  gtk_single_selection_set_can_unselect (self->selection, TRUE);
  g_signal_connect (self->selection, "notify::selected-item", G_CALLBACK (selection_changed), self);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (factory_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (factory_bind), NULL);
  g_signal_connect (factory, "unbind", G_CALLBACK (factory_unbind), NULL);
  GtkWidget *list
      = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (self->selection)), factory);
  gtk_widget_add_css_class (list, "navigation-sidebar");
  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), list);
  gtk_widget_set_vexpand (list_scroll, TRUE);
  gtk_box_append (GTK_BOX (left), list_scroll);
  gtk_widget_set_size_request (left, 260, -1);
  gtk_paned_set_start_child (GTK_PANED (paned), left);
  gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_position (GTK_PANED (paned), 300);
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top (right, 30);
  gtk_widget_set_margin_bottom (right, 30);
  gtk_widget_set_margin_start (right, 24);
  gtk_widget_set_margin_end (right, 24);
  self->name = GTK_LABEL (gtk_label_new (_ ("Select a Virtual Machine")));
  gtk_widget_add_css_class (GTK_WIDGET (self->name), "title-1");
  gtk_label_set_wrap (self->name, TRUE);
  gtk_label_set_xalign (self->name, 0);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->name));
  self->state = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (self->state, 0);
  self->resources = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (self->resources, 0);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->state));
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->resources));
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 3);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 8);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 8);
  const char *labels[]
      = { _ ("Start"),  _ ("Open Console"), _ ("Shut Down"),  _ ("Restart"), _ ("Pause"),
          _ ("Resume"), _ ("Settings"),     _ ("Force Stop"), _ ("Remove") };
  const char *button_actions[]
      = { "win.start",  "win.console", "win.shutdown",   "win.reboot", "win.pause",
          "win.resume", "win.edit",    "win.force-stop", "win.remove" };
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      GtkWidget *button = action_button (labels[i], button_actions[i]);
      if (i >= 7)
        gtk_widget_add_css_class (button, "destructive-action");
      gtk_flow_box_append (GTK_FLOW_BOX (flow), button);
    }
  gtk_box_append (GTK_BOX (right), flow);
  GtkWidget *detail_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (detail_scroll), right);
  gtk_paned_set_end_child (GTK_PANED (paned), detail_scroll);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  gtk_stack_add_named (self->stack, paned, "machines");
  GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  self->operation_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->stage = GTK_LABEL (gtk_label_new (NULL));
  self->progress = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->progress), TRUE);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Cancel"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (cancel_operation), self);
  gtk_box_append (GTK_BOX (self->operation_bar), GTK_WIDGET (self->stage));
  gtk_box_append (GTK_BOX (self->operation_bar), GTK_WIDGET (self->progress));
  gtk_box_append (GTK_BOX (self->operation_bar), cancel);
  gtk_widget_set_visible (self->operation_bar, FALSE);
  gtk_box_append (GTK_BOX (bottom), self->operation_bar);
  self->status = GTK_LABEL (gtk_label_new (_ ("Local virtual machines managed by libvirt")));
  gtk_widget_add_css_class (GTK_WIDGET (self->status), "dim-label");
  gtk_label_set_ellipsize (self->status, PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_bottom (bottom, 8);
  gtk_widget_set_margin_start (bottom, 12);
  gtk_widget_set_margin_end (bottom, 12);
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (self->status));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), bottom);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toolbar);
  gtk_window_set_title (GTK_WINDOW (self), "Pirwa");
  gtk_window_set_default_size (GTK_WINDOW (self), 1040, 680);
  g_signal_connect (self, "close-request", G_CALLBACK (window_close_requested), NULL);
  update_actions (self);
}

GtkWindow *
pirwa_window_new (GtkApplication *application, const char *uri)
{
  PirwaWindow *self = g_object_new (PIRWA_TYPE_WINDOW, "application", application, NULL);
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
  const char *new_accels[] = { "<Primary>n", NULL };
  const char *refresh_accels[] = { "<Primary>r", "F5", NULL };
  gtk_application_set_accels_for_action (application, "win.new", new_accels);
  gtk_application_set_accels_for_action (application, "win.refresh", refresh_accels);
  return GTK_WINDOW (self);
}
