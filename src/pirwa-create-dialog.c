/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-create-dialog.h"
#include "pirwa-avatar.h"
#include <glib/gi18n.h>

enum
{
  SOURCE,
  NAME,
  CONFIGURATION,
  BOOT,
  N_STEPS
};

typedef struct
{
  GtkWidget parent_instance;
  GtkWidget *navigation;
  GtkWidget *steps[N_STEPS];
  double position;
} PirwaStepBar;

typedef GtkWidgetClass PirwaStepBarClass;

G_DEFINE_FINAL_TYPE (PirwaStepBar, pirwa_step_bar, GTK_TYPE_WIDGET)

static void
pirwa_step_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  PirwaStepBar *self = (PirwaStepBar *)widget;
  double position = CLAMP (self->position, SOURCE, BOOT);
  guint index = (guint)position;
  guint next = MIN (index + 1, N_STEPS - 1);
  graphene_rect_t from, to, bounds;
  if (gtk_widget_compute_bounds (self->steps[index], widget, &from)
      && gtk_widget_compute_bounds (self->steps[next], widget, &to))
    {
      graphene_rect_interpolate (&from, &to, position - index, &bounds);
      GdkRGBA color;
      gtk_widget_get_color (widget, &color);
      float gray = (color.red + color.green + color.blue) / 3;
      color = (GdkRGBA){ gray, gray, gray, color.alpha * .12f };
      GskRoundedRect pill;
      gsk_rounded_rect_init_from_rect (&pill, &bounds,
                                       MIN (bounds.size.width, bounds.size.height) / 2);
      gtk_snapshot_push_rounded_clip (snapshot, &pill);
      gtk_snapshot_append_color (snapshot, &color, &bounds);
      gtk_snapshot_pop (snapshot);
    }
  gtk_widget_snapshot_child (widget, self->navigation, snapshot);
}

static void
pirwa_step_bar_dispose (GObject *object)
{
  PirwaStepBar *self = (PirwaStepBar *)object;
  g_clear_pointer (&self->navigation, gtk_widget_unparent);
  G_OBJECT_CLASS (pirwa_step_bar_parent_class)->dispose (object);
}

static void
pirwa_step_bar_class_init (PirwaStepBarClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_step_bar_dispose;
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->snapshot = pirwa_step_bar_snapshot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "pirwastepbar");
}

static void
pirwa_step_bar_init (PirwaStepBar *self)
{
  self->navigation = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_set_homogeneous (GTK_BOX (self->navigation), TRUE);
  gtk_widget_set_parent (self->navigation, GTK_WIDGET (self));
}

struct _PirwaCreateDialog
{
  AdwDialog parent_instance;
  GtkStack *pages;
  GtkScrolledWindow *scrolled;
  GtkWidget *steps[N_STEPS];
  PirwaStepBar *step_bar;
  AdwAnimation *step_animation;
  double step_position;
  GtkWidget *next;
  GtkWidget *browse;
  GtkLabel *drop_title;
  GtkLabel *drop_description;
  AdwEntryRow *name;
  GtkWidget *avatar_picker;
  GtkWidget *summary_avatar;
  AdwSpinRow *cpus;
  AdwSpinRow *memory;
  AdwSpinRow *disk;
  AdwComboRow *pool;
  AdwSwitchRow *import;
  AdwSwitchRow *bios;
  AdwComboRow *guest;
  AdwActionRow *driver_row;
  GtkWidget *driver_browse;
  AdwActionRow *summary_guest;
  AdwActionRow *summary_drivers;
  AdwActionRow *summary_name;
  AdwActionRow *summary_source;
  AdwActionRow *summary_resources;
  AdwActionRow *summary_storage;
  AdwActionRow *summary_firmware;
  GtkLabel *error;
  GCancellable *chooser_cancel;
  char *source;
  char *driver_iso;
  gboolean choosing_driver;
  char *suggested_name;
  GStrv pools;
  guint step;
};

G_DEFINE_FINAL_TYPE (PirwaCreateDialog, pirwa_create_dialog, ADW_TYPE_DIALOG)

static guint signals[2];

static void
show_error (PirwaCreateDialog *self, const char *message)
{
  gtk_label_set_text (self->error, message ? message : "");
  gtk_widget_set_visible (GTK_WIDGET (self->error), message != NULL);
}

static gboolean
valid_name (PirwaCreateDialog *self)
{
  PirwaCreate options = { .name = (char *)gtk_editable_get_text (GTK_EDITABLE (self->name)),
                          .cpus = 1,
                          .memory_mb = 256,
                          .disk_gb = 4 };
  g_autofree char *trimmed = g_strdup (options.name);
  return *g_strstrip (trimmed) && pirwa_config_validate (&options, NULL);
}

static gboolean
has_pool (PirwaCreateDialog *self)
{
  guint index = adw_combo_row_get_selected (self->pool);
  return index != GTK_INVALID_LIST_POSITION && self->pools[index] && *self->pools[index];
}

static void
update_next (PirwaCreateDialog *self)
{
  gboolean valid = self->step == SOURCE ? self->source != NULL
                   : self->step == NAME ? valid_name (self)
                   : self->step == CONFIGURATION
                       ? has_pool (self)
                       : self->source && valid_name (self) && has_pool (self);
  if (self->step >= CONFIGURATION && adw_combo_row_get_selected (self->guest) != PIRWA_GUEST_LINUX)
    valid &= self->driver_iso != NULL;
  gtk_widget_set_sensitive (self->next, valid);
}

static void
input_changed (GObject *object, GParamSpec *spec, PirwaCreateDialog *self)
{
  (void)object;
  (void)spec;
  show_error (self, NULL);
  update_next (self);
}

static gboolean
select_source (PirwaCreateDialog *self, GFile *file)
{
  g_autofree char *path = g_file_get_path (file);
  gboolean import = adw_switch_row_get_active (self->import);
  const char *extension = import ? ".qcow2" : ".iso";
  g_autofree char *lower = path ? g_ascii_strdown (path, -1) : NULL;
  if (!lower || !g_str_has_suffix (lower, extension))
    {
      show_error (self, import ? _ ("Choose a local qcow2 disk image.")
                               : _ ("Choose a local ISO installation image."));
      return FALSE;
    }

  g_autofree char *basename = g_filename_display_basename (path);
  g_autofree char *stem = g_strndup (basename, strlen (basename) - strlen (extension));
  GString *name = g_string_new (NULL);
  for (const char *p = stem; *p && name->len < 320; p = g_utf8_next_char (p))
    {
      gunichar ch = g_utf8_get_char (p);
      if (ch == '_' || ch == '/' || ch == '\\' || g_unichar_iscntrl (ch))
        ch = ' ';
      g_string_append_unichar (name, ch);
    }
  g_strstrip (name->str);
  /* Keep the suggested name within the backend's 80-character limit. */
  if (g_utf8_strlen (name->str, -1) > 80)
    g_string_truncate (name, g_utf8_offset_to_pointer (name->str, 80) - name->str);
  const char *current = gtk_editable_get_text (GTK_EDITABLE (self->name));
  if (!*current || g_strcmp0 (current, self->suggested_name) == 0)
    gtk_editable_set_text (GTK_EDITABLE (self->name),
                           *name->str ? name->str : _ ("New Virtual Machine"));
  g_free (self->suggested_name);
  self->suggested_name = g_strdup (*name->str ? name->str : _ ("New Virtual Machine"));
  g_string_free (name, TRUE);
  g_free (self->source);
  self->source = g_steal_pointer (&path);
  g_autofree char *display_path = g_filename_display_name (self->source);
  gtk_label_set_text (self->drop_title, basename);
  gtk_label_set_text (self->drop_description, display_path);
  gtk_button_set_label (GTK_BUTTON (self->browse), _ ("Choose Another File…"));
  show_error (self, NULL);
  update_next (self);
  return TRUE;
}

static gboolean
source_dropped (GtkDropTarget *target, const GValue *value, double x, double y,
                PirwaCreateDialog *self)
{
  (void)target;
  (void)x;
  (void)y;
  GSList *files = gdk_file_list_get_files (g_value_get_boxed (value));
  if (!files || files->next)
    {
      show_error (self, _ ("Drop one image file at a time."));
      return FALSE;
    }
  return select_source (self, files->data);
}

static gboolean
select_driver (PirwaCreateDialog *self, GFile *file)
{
  g_autofree char *path = g_file_get_path (file);
  g_autofree char *lower = path ? g_ascii_strdown (path, -1) : NULL;
  if (!lower || !g_str_has_suffix (lower, ".iso"))
    {
      show_error (self, _ ("Choose a local virtio-win driver ISO."));
      return FALSE;
    }
  g_free (self->driver_iso);
  self->driver_iso = g_steal_pointer (&path);
  adw_action_row_set_subtitle (self->driver_row, self->driver_iso);
  show_error (self, NULL);
  update_next (self);
  return TRUE;
}

static gboolean
driver_dropped (GtkDropTarget *target, const GValue *value, double x, double y,
                PirwaCreateDialog *self)
{
  (void)target;
  (void)x;
  (void)y;
  GSList *files = gdk_file_list_get_files (g_value_get_boxed (value));
  if (!files || files->next)
    return FALSE;
  return select_driver (self, files->data);
}

static void
file_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (PirwaCreateDialog) self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
  gboolean cancelled = g_cancellable_is_cancelled (self->chooser_cancel);
  g_clear_object (&self->chooser_cancel);
  gtk_widget_set_sensitive (self->browse, TRUE);
  gtk_widget_set_sensitive (self->driver_browse, TRUE);
  if (cancelled)
    return;
  if (file)
    {
      if (self->choosing_driver)
        select_driver (self, file);
      else
        select_source (self, file);
    }
  else if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
           && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    show_error (self, error->message);
}

static void
choose_source (GtkButton *button, PirwaCreateDialog *self)
{
  if (self->chooser_cancel)
    return;
  self->choosing_driver = GTK_WIDGET (button) == self->driver_browse;
  gboolean import = !self->choosing_driver && adw_switch_row_get_active (self->import);
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, import ? _ ("Choose a Disk Image") : _ ("Choose an ISO"));
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, import ? _ ("qcow2 Disk Images") : _ ("ISO Images"));
  gtk_file_filter_add_suffix (filter, import ? "qcow2" : "iso");
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  self->chooser_cancel = g_cancellable_new ();
  gtk_widget_set_sensitive (self->browse, FALSE);
  gtk_widget_set_sensitive (self->driver_browse, FALSE);
  gtk_file_dialog_open (dialog, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                        self->chooser_cancel, file_chosen, g_object_ref (self));
}

static void
import_changed (GObject *object, GParamSpec *spec, PirwaCreateDialog *self)
{
  (void)object;
  (void)spec;
  gboolean import = adw_switch_row_get_active (self->import);
  g_clear_pointer (&self->source, g_free);
  gtk_label_set_text (self->drop_title,
                      import ? _ ("Drop your disk image here") : _ ("Drop your ISO here"));
  gtk_label_set_text (self->drop_description,
                      import ? _ ("Import a standalone qcow2 image.")
                             : _ ("Choose a bootable Linux or Windows installation image."));
  gtk_button_set_label (GTK_BUTTON (self->browse),
                        import ? _ ("Choose a Disk Image…") : _ ("Choose an ISO…"));
  gtk_widget_set_sensitive (GTK_WIDGET (self->disk), !import);
  if (import)
    adw_combo_row_set_selected (self->guest, PIRWA_GUEST_LINUX);
  gtk_widget_set_sensitive (GTK_WIDGET (self->guest), !import);
  show_error (self, NULL);
  update_next (self);
}

static void
guest_changed (GObject *object, GParamSpec *spec, PirwaCreateDialog *self)
{
  (void)object;
  (void)spec;
  PirwaGuest guest = adw_combo_row_get_selected (self->guest);
  gboolean windows = guest != PIRWA_GUEST_LINUX;
  gtk_widget_set_visible (GTK_WIDGET (self->driver_row), windows);
  if (windows)
    {
      adw_spin_row_set_value (self->cpus, MAX (2, adw_spin_row_get_value (self->cpus)));
      adw_spin_row_set_value (self->memory, MAX (4096, adw_spin_row_get_value (self->memory)));
      adw_spin_row_set_value (self->disk, MAX (64, adw_spin_row_get_value (self->disk)));
    }
  if (guest == PIRWA_GUEST_WINDOWS_11)
    adw_switch_row_set_active (self->bios, FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->bios), guest != PIRWA_GUEST_WINDOWS_11);
  show_error (self, NULL);
  update_next (self);
}

static void
update_summary (PirwaCreateDialog *self)
{
  pirwa_avatar_set_preset (self->summary_avatar, pirwa_create_dialog_get_avatar (self));
  PirwaGuest guest = adw_combo_row_get_selected (self->guest);
  GtkStringObject *item = GTK_STRING_OBJECT (adw_combo_row_get_selected_item (self->guest));
  adw_action_row_set_subtitle (self->summary_guest, gtk_string_object_get_string (item));
  gtk_widget_set_visible (GTK_WIDGET (self->summary_drivers), guest != PIRWA_GUEST_LINUX);
  adw_action_row_set_subtitle (self->summary_drivers, self->driver_iso);
  adw_action_row_set_subtitle (self->summary_name,
                               gtk_editable_get_text (GTK_EDITABLE (self->name)));
  g_autofree char *basename = g_filename_display_basename (self->source);
  adw_action_row_set_subtitle (self->summary_source, basename);
  g_autofree char *resources
      = g_strdup_printf (_ ("%u CPUs · %u MiB RAM"), (guint)adw_spin_row_get_value (self->cpus),
                         (guint)adw_spin_row_get_value (self->memory));
  adw_action_row_set_subtitle (self->summary_resources, resources);
  const char *pool = self->pools[adw_combo_row_get_selected (self->pool)];
  g_autofree char *storage
      = adw_switch_row_get_active (self->import)
            ? g_strdup_printf (_ ("Original disk capacity · %s"), pool)
            : g_strdup_printf (_ ("%u GiB · %s"), (guint)adw_spin_row_get_value (self->disk), pool);
  adw_action_row_set_subtitle (self->summary_storage, storage);
  adw_action_row_set_subtitle (
      self->summary_firmware, guest == PIRWA_GUEST_WINDOWS_11 ? _ ("UEFI · Secure Boot · TPM 2.0")
                              : adw_switch_row_get_active (self->bios) ? _ ("Legacy BIOS")
                                                                       : _ ("UEFI when available"));
}

static void
step_highlight_changed (double value, gpointer data)
{
  PirwaCreateDialog *self = data;
  self->step_position = value;
  self->step_bar->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self->step_bar));
}

static void
show_step (PirwaCreateDialog *self, guint step)
{
  if (self->step != step)
    {
      adw_timed_animation_set_value_from (ADW_TIMED_ANIMATION (self->step_animation),
                                          self->step_position);
      adw_timed_animation_set_value_to (ADW_TIMED_ANIMATION (self->step_animation), step);
      adw_animation_play (self->step_animation);
    }
  self->step = step;
  for (guint i = 0; i < N_STEPS; i++)
    {
      gtk_widget_set_sensitive (self->steps[i], i <= step);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->steps[i]),
                                      GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                      i == step ? _ ("Current step") : NULL, -1);
    }
  const char *names[] = { "source", "name", "configuration", "boot" };
  gtk_stack_set_visible_child_name (self->pages, names[step]);
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (self->scrolled);
  gtk_adjustment_set_value (adjustment, 0);
  gtk_button_set_label (GTK_BUTTON (self->next),
                        step == BOOT ? _ ("Create and Boot") : _ ("Continue"));
  show_error (self, NULL);
  update_next (self);
  if (step == BOOT)
    update_summary (self);
  if (step == NAME)
    gtk_widget_grab_focus (GTK_WIDGET (self->name));
  else if (step == CONFIGURATION)
    gtk_widget_grab_focus (GTK_WIDGET (self->cpus));
  else
    gtk_widget_grab_focus (step == SOURCE ? self->browse : self->next);
}

static void
go_to_step (GtkButton *button, PirwaCreateDialog *self)
{
  guint step = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "step"));
  if (adw_dialog_get_can_close (ADW_DIALOG (self)) && step < self->step)
    show_step (self, step);
}

static void
go_next (GtkButton *button, PirwaCreateDialog *self)
{
  (void)button;
  if (!adw_dialog_get_can_close (ADW_DIALOG (self)) || !gtk_widget_get_sensitive (self->next))
    return;
  if (self->step < BOOT)
    {
      show_step (self, self->step + 1);
      return;
    }
  PirwaCreate options
      = { .name = (char *)gtk_editable_get_text (GTK_EDITABLE (self->name)),
          .source = self->source,
          .pool = self->pools[adw_combo_row_get_selected (self->pool)],
          .cpus = adw_spin_row_get_value (self->cpus),
          .memory_mb = adw_spin_row_get_value (self->memory),
          .disk_gb = adw_spin_row_get_value (self->disk),
          .import_image = adw_switch_row_get_active (self->import),
          .legacy_bios = adw_switch_row_get_active (self->bios),
          .guest = adw_combo_row_get_selected (self->guest),
          .driver_iso = adw_combo_row_get_selected (self->guest) != PIRWA_GUEST_LINUX
                            ? self->driver_iso
                            : NULL };
  g_autoptr (GError) error = NULL;
  if (!pirwa_config_validate (&options, &error))
    {
      show_error (self, error->message);
      return;
    }
  show_error (self, NULL);
  GtkProgressBar *progress = g_object_get_data (G_OBJECT (self), "progress");
  gtk_progress_bar_set_fraction (progress, 0);
  gtk_progress_bar_set_text (progress, _ ("Preparing machine…"));
  g_signal_emit (self, signals[0], 0, &options);
}

static void
cancel_clicked (GtkButton *button, PirwaCreateDialog *self)
{
  (void)button;
  if (adw_dialog_get_can_close (ADW_DIALOG (self)))
    adw_dialog_close (ADW_DIALOG (self));
  else
    g_signal_emit (self, signals[1], 0);
}

static void
dialog_closed (AdwDialog *dialog, PirwaCreateDialog *self)
{
  (void)dialog;
  if (self->chooser_cancel)
    g_cancellable_cancel (self->chooser_cancel);
}

static GtkWidget *
page_new (PirwaCreateDialog *self, const char *name, const char *title, const char *description)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  GtkWidget *heading = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *label = gtk_label_new (title);
  gtk_widget_add_css_class (label, "title-1");
  gtk_widget_add_css_class (label, "pirwa-create-title");
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_box_append (GTK_BOX (heading), label);
  label = gtk_label_new (description);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (heading), label);
  gtk_box_append (GTK_BOX (page), heading);
  gtk_stack_add_named (self->pages, page, name);
  return page;
}

static AdwActionRow *
summary_row (GtkWidget *group, const char *title)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (row));
  return row;
}

static AdwSpinRow *
spin_row (GtkWidget *group, const char *title, double value, double lower, double upper,
          double step)
{
  AdwSpinRow *row = ADW_SPIN_ROW (adw_spin_row_new_with_range (lower, upper, step));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_spin_row_set_value (row, value);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (row));
  return row;
}

static void
build_source_page (PirwaCreateDialog *self)
{
  GtkWidget *page = page_new (self, "source", _ ("Choose an Installation Image"),
                              _ ("Start with a local ISO to install your operating system."));
  GtkWidget *drop = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (drop, "pirwa-source-drop");
  GtkWidget *icon = gtk_image_new_from_icon_name ("media-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
  gtk_widget_add_css_class (icon, "accent");
  gtk_box_append (GTK_BOX (drop), icon);
  self->drop_title = GTK_LABEL (gtk_label_new (_ ("Drop your ISO here")));
  gtk_widget_add_css_class (GTK_WIDGET (self->drop_title), "title-2");
  gtk_label_set_wrap (self->drop_title, TRUE);
  gtk_label_set_wrap_mode (self->drop_title, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (self->drop_title, GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (drop), GTK_WIDGET (self->drop_title));
  self->drop_description
      = GTK_LABEL (gtk_label_new (_ ("Choose a bootable Linux or Windows installation image.")));
  gtk_label_set_wrap (self->drop_description, TRUE);
  gtk_label_set_wrap_mode (self->drop_description, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (self->drop_description, GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->drop_description), "dim-label");
  gtk_box_append (GTK_BOX (drop), GTK_WIDGET (self->drop_description));
  self->browse = gtk_button_new_with_label (_ ("Choose an ISO…"));
  gtk_widget_add_css_class (self->browse, "pill");
  gtk_widget_set_halign (self->browse, GTK_ALIGN_CENTER);
  g_signal_connect (self->browse, "clicked", G_CALLBACK (choose_source), self);
  gtk_box_append (GTK_BOX (drop), self->browse);
  GtkDropTarget *target = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (target, "drop", G_CALLBACK (source_dropped), self);
  gtk_widget_add_controller (drop, GTK_EVENT_CONTROLLER (target));
  gtk_box_append (GTK_BOX (page), drop);
  GtkWidget *group = adw_preferences_group_new ();
  self->import = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->import), _ ("Import an Existing Disk"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->import),
                               _ ("Copy a standalone qcow2 image into a new machine."));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->import));
  g_signal_connect (self->import, "notify::active", G_CALLBACK (import_changed), self);
  gtk_box_append (GTK_BOX (page), group);
}

static void
build_name_page (PirwaCreateDialog *self)
{
  GtkWidget *page = page_new (self, "name", _ ("Name Your Machine"),
                              _ ("We’ve suggested a name from your image. Make it your own."));
  GtkWidget *group = adw_preferences_group_new ();
  self->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name), _ ("Name"));
  adw_entry_row_set_activates_default (self->name, TRUE);
  gtk_editable_set_max_width_chars (GTK_EDITABLE (self->name), 40);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->name));
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                         _ ("Use 1–80 characters without slashes."));
  g_signal_connect (self->name, "notify::text", G_CALLBACK (input_changed), self);
  gtk_box_append (GTK_BOX (page), group);
  GtkWidget *appearance = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (appearance), _ ("Machine Icon"));
  self->avatar_picker = pirwa_avatar_picker_new (NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (appearance), self->avatar_picker);
  gtk_box_append (GTK_BOX (page), appearance);
}

static void
build_configuration_page (PirwaCreateDialog *self)
{
  GtkWidget *page = page_new (self, "configuration", _ ("Configure Your Machine"),
                              _ ("Adjust its resources before the first boot."));
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _ ("Guest Operating System"));
  adw_preferences_group_set_description (
      ADW_PREFERENCES_GROUP (group),
      _ ("Windows support is experimental. During Windows Setup, load the storage driver "
         "from the virtio-win ISO, then install its network and guest tools."));
  self->guest = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->guest), _ ("Operating system"));
  const char *guests[] = { _ ("Linux"), _ ("Windows 10"), _ ("Windows 11"), NULL };
  g_autoptr (GtkStringList) guest_model = gtk_string_list_new (guests);
  adw_combo_row_set_model (self->guest, G_LIST_MODEL (guest_model));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->guest));
  self->driver_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->driver_row), _ ("VirtIO Driver ISO"));
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->driver_row), FALSE);
  adw_action_row_set_subtitle (self->driver_row, _ ("Choose or drop a local virtio-win ISO."));
  self->driver_browse = gtk_button_new_with_label (_ ("Choose…"));
  gtk_widget_set_valign (self->driver_browse, GTK_ALIGN_CENTER);
  g_signal_connect (self->driver_browse, "clicked", G_CALLBACK (choose_source), self);
  adw_action_row_add_suffix (self->driver_row, self->driver_browse);
  adw_action_row_set_activatable_widget (self->driver_row, self->driver_browse);
  GtkDropTarget *target = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (target, "drop", G_CALLBACK (driver_dropped), self);
  gtk_widget_add_controller (GTK_WIDGET (self->driver_row), GTK_EVENT_CONTROLLER (target));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->driver_row));
  gtk_widget_set_visible (GTK_WIDGET (self->driver_row), FALSE);
  gtk_box_append (GTK_BOX (page), group);
  group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _ ("Resources"));
  self->cpus = spin_row (group, _ ("Virtual CPUs"), 2, 1, 64, 1);
  self->memory = spin_row (group, _ ("Memory (MiB)"), 2048, 256, 262144, 256);
  self->disk = spin_row (group, _ ("Disk capacity (GiB)"), 32, 4, 2048, 1);
  gtk_box_append (GTK_BOX (page), group);
  group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _ ("Storage and Firmware"));
  self->pool = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->pool), _ ("Directory storage pool"));
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->pool), FALSE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->pool));
  g_signal_connect (self->pool, "notify::selected", G_CALLBACK (input_changed), self);
  self->bios = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->bios), _ ("Use Legacy BIOS"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->bios),
                               _ ("Enable for an image installed with legacy firmware."));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (self->bios));
  gtk_box_append (GTK_BOX (page), group);
  g_signal_connect (self->guest, "notify::selected", G_CALLBACK (guest_changed), self);
}

static void
build_boot_page (PirwaCreateDialog *self)
{
  GtkWidget *page = page_new (self, "boot", _ ("Ready to Boot"),
                              _ ("Create your machine and boot it right away."));
  GtkWidget *group = adw_preferences_group_new ();
  self->summary_name = summary_row (group, _ ("Name"));
  self->summary_avatar = pirwa_avatar_new (NULL, 40);
  adw_action_row_add_prefix (self->summary_name, self->summary_avatar);
  self->summary_source = summary_row (group, _ ("Image"));
  self->summary_guest = summary_row (group, _ ("Operating system"));
  self->summary_drivers = summary_row (group, _ ("Driver ISO"));
  self->summary_resources = summary_row (group, _ ("Resources"));
  self->summary_storage = summary_row (group, _ ("Storage"));
  self->summary_firmware = summary_row (group, _ ("Firmware"));
  gtk_box_append (GTK_BOX (page), group);
  GtkWidget *hint = gtk_label_new (_ ("A new disk is allocated in the selected pool. Imported "
                                      "images keep their original capacity and are copied; the "
                                      "source stays unchanged."));
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (page), hint);
}

static void
pirwa_create_dialog_dispose (GObject *object)
{
  PirwaCreateDialog *self = PIRWA_CREATE_DIALOG (object);
  if (self->step_animation)
    adw_animation_pause (self->step_animation);
  g_clear_object (&self->step_animation);
  G_OBJECT_CLASS (pirwa_create_dialog_parent_class)->dispose (object);
}

static void
pirwa_create_dialog_finalize (GObject *object)
{
  PirwaCreateDialog *self = PIRWA_CREATE_DIALOG (object);
  g_clear_object (&self->chooser_cancel);
  g_free (self->source);
  g_free (self->driver_iso);
  g_free (self->suggested_name);
  g_strfreev (self->pools);
  G_OBJECT_CLASS (pirwa_create_dialog_parent_class)->finalize (object);
}

static void
pirwa_create_dialog_class_init (PirwaCreateDialogClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_create_dialog_dispose;
  G_OBJECT_CLASS (klass)->finalize = pirwa_create_dialog_finalize;
  signals[0] = g_signal_new ("create-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[1] = g_signal_new ("cancel-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                             NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
pirwa_create_dialog_init (PirwaCreateDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), _ ("New Virtual Machine"));
  adw_dialog_set_content_width (ADW_DIALOG (self), 600);
  adw_dialog_set_content_height (ADW_DIALOG (self), 660);
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (!g_object_get_data (G_OBJECT (display), "pirwa-create-styles"))
    {
      g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (
          css,
          ".pirwa-create-title { font-weight: 700; }"
          ".pirwa-source-drop { border: 2px dashed alpha(@accent_color, .4);"
          "border-radius: 18px; background: alpha(@accent_bg_color, .06); padding: 28px 16px; }"
          ".pirwa-source-drop:drop(active) { border-color: @accent_color;"
          "background: alpha(@accent_bg_color, .15); }");
      gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_set_data (G_OBJECT (display), "pirwa-create-styles", GINT_TO_POINTER (1));
    }
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Cancel"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (cancel_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  self->next = gtk_button_new_with_label (_ ("Continue"));
  gtk_widget_add_css_class (self->next, "suggested-action");
  g_signal_connect (self->next, "clicked", G_CALLBACK (go_next), self);
  g_object_bind_property (self, "can-close", self->next, "sensitive", G_BINDING_SYNC_CREATE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->next);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_start (content, 24);
  gtk_widget_set_margin_end (content, 24);
  gtk_widget_set_margin_top (content, 12);
  gtk_widget_set_margin_bottom (content, 24);
  self->pages = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_vhomogeneous (self->pages, FALSE);
  build_source_page (self);
  build_name_page (self);
  build_configuration_page (self);
  build_boot_page (self);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->pages));
  self->scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (self->scrolled, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (self->scrolled, content);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->scrolled));
  g_object_bind_property (self, "can-close", content, "sensitive", G_BINDING_SYNC_CREATE);

  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (footer, 24);
  gtk_widget_set_margin_end (footer, 24);
  gtk_widget_set_margin_top (footer, 12);
  gtk_widget_set_margin_bottom (footer, 18);
  self->error = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->error, TRUE);
  gtk_label_set_wrap_mode (self->error, PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (self->error), "error");
  gtk_widget_set_visible (GTK_WIDGET (self->error), FALSE);
  gtk_box_append (GTK_BOX (footer), GTK_WIDGET (self->error));
  GtkWidget *progress = gtk_progress_bar_new ();
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (progress), TRUE);
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progress), _ ("Preparing machine…"));
  g_object_bind_property (self, "can-close", progress, "visible",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
  gtk_box_append (GTK_BOX (footer), progress);
  self->step_bar = g_object_new (pirwa_step_bar_get_type (), NULL);
  GtkWidget *navigation = self->step_bar->navigation;
  AdwAnimationTarget *target
      = adw_callback_animation_target_new (step_highlight_changed, self, NULL);
  self->step_animation
      = adw_timed_animation_new (GTK_WIDGET (self->step_bar), SOURCE, SOURCE, 240, target);
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->step_animation), ADW_EASE_OUT_CUBIC);
  const char *labels[] = { _ ("1 Image"), _ ("2 Name"), _ ("3 Config"), _ ("4 Boot") };
  for (guint i = 0; i < N_STEPS; i++)
    {
      self->steps[i] = gtk_button_new_with_label (labels[i]);
      self->step_bar->steps[i] = self->steps[i];
      GtkLabel *label = GTK_LABEL (gtk_button_get_child (GTK_BUTTON (self->steps[i])));
      gtk_label_set_wrap (label, TRUE);
      gtk_label_set_justify (label, GTK_JUSTIFY_CENTER);
      gtk_widget_add_css_class (self->steps[i], "pill");
      gtk_widget_add_css_class (self->steps[i], "flat");
      gtk_widget_add_css_class (self->steps[i], "caption-heading");
      g_object_set_data (G_OBJECT (self->steps[i]), "step", GUINT_TO_POINTER (i));
      g_signal_connect (self->steps[i], "clicked", G_CALLBACK (go_to_step), self);
      gtk_box_append (GTK_BOX (navigation), self->steps[i]);
    }
  g_object_bind_property (self, "can-close", navigation, "sensitive", G_BINDING_SYNC_CREATE);
  gtk_box_append (GTK_BOX (footer), GTK_WIDGET (self->step_bar));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), footer);
  g_object_set_data (G_OBJECT (self), "apply", self->next);
  g_object_set_data (G_OBJECT (self), "error", self->error);
  g_object_set_data (G_OBJECT (self), "progress", progress);
  g_signal_connect (self, "closed", G_CALLBACK (dialog_closed), self);
  adw_dialog_set_child (ADW_DIALOG (self), toolbar);
  adw_dialog_set_default_widget (ADW_DIALOG (self), self->next);
}

AdwDialog *
pirwa_create_dialog_new (const char *const *pools, gboolean session)
{
  PirwaCreateDialog *self = g_object_new (PIRWA_TYPE_CREATE_DIALOG, NULL);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  if (session)
    g_ptr_array_add (names, g_strdup ("pirwa"));
  for (guint i = 0; pools && pools[i]; i++)
    if (!session || !g_str_equal (pools[i], "pirwa"))
      g_ptr_array_add (names, g_strdup (pools[i]));
  g_ptr_array_add (names, NULL);
  self->pools = (GStrv)g_ptr_array_free (names, FALSE);
  g_autoptr (GtkStringList) model = gtk_string_list_new ((const char *const *)self->pools);
  adw_combo_row_set_model (self->pool, G_LIST_MODEL (model));
  if (!self->pools[0])
    adw_action_row_set_subtitle (ADW_ACTION_ROW (self->pool),
                                 _ ("No active directory pool. Add one in libvirt, then refresh."));
  show_step (self, SOURCE);
  return ADW_DIALOG (self);
}

const char *
pirwa_create_dialog_get_avatar (PirwaCreateDialog *self)
{
  return pirwa_avatar_picker_get_preset (self->avatar_picker);
}
