/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA; no guest, daemon, KVM, or source image required. */
#include "pirwa-avatar.h"
#include "pirwa-create-dialog.h"
#include <glib/gstdio.h>
#include <locale.h>

static char *output;
static int width = 1040;

static void
settle (void)
{
  gint64 until = g_get_monotonic_time () + 350000;
  while (g_get_monotonic_time () < until)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
}

static GtkWidget *
find_type (GtkWidget *root, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_type (child, type);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_row (GtkWidget *root, const char *title)
{
  if (gtk_widget_get_mapped (root) && ADW_IS_PREFERENCES_ROW (root)
      && g_strcmp0 (adw_preferences_row_get_title (ADW_PREFERENCES_ROW (root)), title) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_row (child, title);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_button (GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON (root))
    {
      GtkWidget *text = find_type (root, GTK_TYPE_LABEL);
      if (text && g_strcmp0 (gtk_label_get_text (GTK_LABEL (text)), label) == 0)
        return root;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_button (child, label);
      if (found)
        return found;
    }
  return NULL;
}

static GtkDropTarget *
find_drop (GtkWidget *root)
{
  g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (root);
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
      g_autoptr (GObject) controller = g_list_model_get_item (controllers, i);
      if (GTK_IS_DROP_TARGET (controller))
        return g_object_ref (GTK_DROP_TARGET (controller));
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkDropTarget *found = find_drop (child);
      if (found)
        return found;
    }
  return NULL;
}

static gboolean
drop_at (GtkWidget *root, const char *uri, gboolean multiple)
{
  g_autoptr (GObject) target = G_OBJECT (find_drop (root));
  g_assert_nonnull (target);
  g_autoptr (GFile) file = g_file_new_for_uri (uri);
  GFile *files[] = { file, file };
  GValue value = G_VALUE_INIT;
  g_value_init (&value, GDK_TYPE_FILE_LIST);
  g_value_take_boxed (&value, gdk_file_list_new_from_array (files, multiple ? 2 : 1));
  gboolean accepted = FALSE;
  g_signal_emit_by_name (target, "drop", &value, 0., 0., &accepted);
  g_value_unset (&value);
  return accepted;
}

static gboolean
drop (AdwDialog *dialog, const char *uri, gboolean multiple)
{
  return drop_at (GTK_WIDGET (dialog), uri, multiple);
}

static void
assert_step_alignment (AdwDialog *dialog)
{
  const char *labels[] = { "Image", "Name", "Config", "Boot" };
  GtkWidget *first = find_button (GTK_WIDGET (dialog), labels[0]);
  g_assert_nonnull (first);
  GtkWidget *bar = gtk_widget_get_parent (gtk_widget_get_parent (first));
  graphene_rect_t reference;
  g_assert_true (gtk_widget_compute_bounds (first, bar, &reference));
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      GtkWidget *button = find_button (GTK_WIDGET (dialog), labels[i]);
      g_assert_nonnull (button);
      graphene_rect_t bounds;
      g_assert_true (gtk_widget_compute_bounds (button, bar, &bounds));
      g_assert_cmpfloat (bounds.origin.y, >=, 0);
      g_assert_cmpfloat (bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height (bar) + 1);
      g_assert_cmpfloat_with_epsilon (bounds.origin.y, reference.origin.y, 1);
      g_assert_cmpfloat_with_epsilon (bounds.size.height, reference.size.height, 1);
    }
}

static void
click (AdwDialog *dialog, const char *label)
{
  GtkWidget *button = find_button (GTK_WIDGET (dialog), label);
  g_assert_nonnull (button);
  g_assert_true (gtk_widget_is_sensitive (button));
  g_assert_true (gtk_widget_get_mapped (button));
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (button, GTK_WIDGET (dialog), &bounds));
  g_assert_cmpfloat (bounds.origin.x, >=, 0);
  g_assert_cmpfloat (bounds.origin.y, >=, 0);
  g_assert_cmpfloat (bounds.origin.x + bounds.size.width, <=,
                     gtk_widget_get_width (GTK_WIDGET (dialog)) + 1);
  g_assert_cmpfloat (bounds.origin.y + bounds.size.height, <=,
                     gtk_widget_get_height (GTK_WIDGET (dialog)) + 1);
  g_signal_emit_by_name (button, "clicked");
  settle ();
  if (gtk_widget_get_mapped (GTK_WIDGET (dialog)))
    assert_step_alignment (dialog);
}

static void
snapshot (GtkWindow *window, const char *stage)
{
  if (!output)
    return;
  g_assert_cmpint (g_mkdir_with_parents (output, 0700), ==, 0);
  g_autofree char *path = g_strdup_printf ("%s/%s.png", output, stage);
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (window));
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot),
                          gtk_widget_get_width (GTK_WIDGET (window)),
                          gtk_widget_get_height (GTK_WIDGET (window)));
  GskRenderNode *node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  g_autoptr (GdkTexture) texture
      = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node, NULL);
  gsk_render_node_unref (node);
  g_assert_true (gdk_texture_save_to_png (texture, path));
}

static GtkWindow *
present (AdwDialog *dialog)
{
  GtkWindow *window = GTK_WINDOW (adw_window_new ());
  gtk_window_set_default_size (window, width, 740);
  gtk_window_present (window);
  adw_dialog_present (dialog, GTK_WIDGET (window));
  settle ();
  settle ();
  g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (dialog)));
  assert_step_alignment (dialog);
  return window;
}

static void
create_requested (PirwaCreateDialog *dialog, PirwaCreate *options, guint *requests)
{
  g_assert_cmpstr (pirwa_create_dialog_get_avatar (dialog), ==, "cheerful");
  g_assert_cmpstr (options->name, ==, "My Fedora <test> & VM");
  g_assert_cmpstr (options->source, ==, "/pirwa-ui-fixture/Replacement.iso");
  g_assert_cmpstr (options->pool, ==, "pirwa");
  g_assert_cmpuint (options->cpus, ==, 4);
  g_assert_cmpuint (options->memory_mb, ==, 4096);
  g_assert_cmpuint (options->disk_gb, ==, 48);
  g_assert_false (options->import_image);
  g_assert_false (options->legacy_bios);
  (*requests)++;
  adw_dialog_set_can_close (ADW_DIALOG (dialog), FALSE);
}

static void
cancel_requested (PirwaCreateDialog *dialog, guint *cancels)
{
  (void)dialog;
  (*cancels)++;
}

static void
test_wizard (void)
{
  const char *pools[] = { "pirwa", "other <pool>", NULL };
  g_autoptr (AdwDialog) dialog = g_object_ref_sink (pirwa_create_dialog_new (pools, TRUE));
  GtkWindow *window = present (dialog);
  GtkWidget *next = g_object_get_data (G_OBJECT (dialog), "apply");
  g_assert_false (gtk_widget_is_sensitive (next));
  g_assert_false (gtk_widget_is_sensitive (find_button (GTK_WIDGET (dialog), "Name")));
  g_assert_false (gtk_widget_is_sensitive (find_button (GTK_WIDGET (dialog), "Config")));
  g_assert_false (gtk_widget_is_sensitive (find_button (GTK_WIDGET (dialog), "Boot")));
  snapshot (window, "1-image");
  g_assert_false (drop (dialog, "file:///pirwa-ui-fixture/Fedora.iso", TRUE));
  g_assert_false (drop (dialog, "https://example.org/Fedora.iso", FALSE));
  g_assert_false (drop (dialog, "file:///pirwa-ui-fixture/Fedora.txt", FALSE));
  g_assert_false (gtk_widget_is_sensitive (next));
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Fedora_Workstation_43.ISO", FALSE));
  settle ();
  snapshot (window, "1-image-selected");
  click (dialog, "Continue");
  GtkEditable *name = GTK_EDITABLE (find_row (GTK_WIDGET (dialog), "Name"));
  g_assert_cmpstr (gtk_editable_get_text (name), ==, "Fedora Workstation 43");
  gtk_editable_set_text (name, "");
  g_assert_false (gtk_widget_is_sensitive (next));
  gtk_editable_set_text (name, "   ");
  g_assert_false (gtk_widget_is_sensitive (next));
  gtk_editable_set_text (name, "bad/name");
  g_assert_false (gtk_widget_is_sensitive (next));
  gtk_editable_set_text (name, "My Fedora <test> & VM");
  GtkWidget *picker = find_row (GTK_WIDGET (dialog), "Choose Icon");
  g_assert_nonnull (picker);
  adw_expander_row_set_expanded (ADW_EXPANDER_ROW (picker), TRUE);
  GtkFlowBox *flow = GTK_FLOW_BOX (find_type (picker, GTK_TYPE_FLOW_BOX));
  g_assert_nonnull (flow);
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (flow)); child;
       child = gtk_widget_get_next_sibling (child))
    if (g_strcmp0 (g_object_get_data (G_OBJECT (child), "avatar-id"), "cheerful") == 0)
      g_signal_emit_by_name (flow, "child-activated", child);
  g_assert_cmpstr (pirwa_create_dialog_get_avatar (PIRWA_CREATE_DIALOG (dialog)), ==, "cheerful");
  settle ();
  snapshot (window, "2-name");
  GtkWidget *image_step = find_button (GTK_WIDGET (dialog), "Image");
  gtk_widget_set_state_flags (image_step, GTK_STATE_FLAG_PRELIGHT, FALSE);
  settle ();
  assert_step_alignment (dialog);
  snapshot (window, "2-name-hover");
  gtk_widget_unset_state_flags (image_step, GTK_STATE_FLAG_PRELIGHT);
  click (dialog, "Image");
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Replacement.iso", FALSE));
  click (dialog, "Continue");
  g_assert_cmpstr (gtk_editable_get_text (name), ==, "My Fedora <test> & VM");
  click (dialog, "Continue");
  adw_spin_row_set_value (ADW_SPIN_ROW (find_row (GTK_WIDGET (dialog), "Virtual CPUs")), 4);
  adw_spin_row_set_value (ADW_SPIN_ROW (find_row (GTK_WIDGET (dialog), "Memory (MiB)")), 4096);
  adw_spin_row_set_value (ADW_SPIN_ROW (find_row (GTK_WIDGET (dialog), "Disk capacity (GiB)")), 48);
  settle ();
  snapshot (window, "3-configuration");
  click (dialog, "Continue");
  g_assert_cmpstr (
      adw_action_row_get_subtitle (ADW_ACTION_ROW (find_row (GTK_WIDGET (dialog), "Name"))), ==,
      "My Fedora <test> & VM");
  snapshot (window, "4-boot");
  click (dialog, "Image");
  click (dialog, "Continue");
  click (dialog, "Continue");
  g_assert_cmpfloat (
      adw_spin_row_get_value (ADW_SPIN_ROW (find_row (GTK_WIDGET (dialog), "Virtual CPUs"))), ==,
      4);
  click (dialog, "Continue");
  guint requests = 0, cancels = 0;
  g_signal_connect (dialog, "create-requested", G_CALLBACK (create_requested), &requests);
  g_signal_connect (dialog, "cancel-requested", G_CALLBACK (cancel_requested), &cancels);
  click (dialog, "Create and Boot");
  g_assert_cmpuint (requests, ==, 1);
  g_assert_false (gtk_widget_is_sensitive (next));
  const char *steps[] = { "Image", "Name", "Config", "Boot" };
  for (guint i = 0; i < G_N_ELEMENTS (steps); i++)
    g_assert_false (gtk_widget_is_sensitive (find_button (GTK_WIDGET (dialog), steps[i])));
  g_signal_emit_by_name (next, "clicked");
  g_assert_cmpuint (requests, ==, 1);
  click (dialog, "Cancel");
  g_assert_cmpuint (cancels, ==, 1);
  g_assert_false (adw_dialog_get_can_close (dialog));
  /* Simulate a failed backend operation. The existing window restores these. */
  adw_dialog_set_can_close (dialog, TRUE);
  gtk_widget_set_sensitive (next, TRUE);
  GtkLabel *error = g_object_get_data (G_OBJECT (dialog), "error");
  gtk_label_set_text (error, "Could not create the machine. Try again.");
  gtk_widget_set_visible (GTK_WIDGET (error), TRUE);
  settle ();
  snapshot (window, "5-error-retry");
  click (dialog, "Create and Boot");
  g_assert_cmpuint (requests, ==, 2);
  adw_dialog_set_can_close (dialog, TRUE);
  click (dialog, "Cancel");
  g_assert_false (gtk_widget_get_mapped (GTK_WIDGET (dialog)));
  gtk_window_destroy (window);
}

static void
test_import (void)
{
  g_autoptr (AdwDialog) dialog = g_object_ref_sink (pirwa_create_dialog_new (NULL, TRUE));
  GtkWindow *window = present (dialog);
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Original.iso", FALSE));
  click (dialog, "Continue");
  GtkEditable *name = GTK_EDITABLE (find_row (GTK_WIDGET (dialog), "Name"));
  g_assert_cmpstr (gtk_editable_get_text (name), ==, "Original");
  click (dialog, "Image");
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Another.iso", FALSE));
  click (dialog, "Continue");
  g_assert_cmpstr (gtk_editable_get_text (name), ==, "Another");
  click (dialog, "Image");
  adw_switch_row_set_active (
      ADW_SWITCH_ROW (find_row (GTK_WIDGET (dialog), "Import an Existing Disk")), TRUE);
  g_assert_false (gtk_widget_is_sensitive (g_object_get_data (G_OBJECT (dialog), "apply")));
  g_assert_false (drop (dialog, "file:///pirwa-ui-fixture/Another.iso", FALSE));
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Debian.qcow2", FALSE));
  click (dialog, "Continue");
  g_assert_cmpstr (gtk_editable_get_text (name), ==, "Debian");
  click (dialog, "Continue");
  g_assert_false (gtk_widget_is_sensitive (find_row (GTK_WIDGET (dialog), "Disk capacity (GiB)")));
  adw_switch_row_set_active (ADW_SWITCH_ROW (find_row (GTK_WIDGET (dialog), "Use Legacy BIOS")),
                             TRUE);
  click (dialog, "Continue");
  g_assert_cmpstr (
      adw_action_row_get_subtitle (ADW_ACTION_ROW (find_row (GTK_WIDGET (dialog), "Storage"))), ==,
      "Original disk capacity · pirwa");
  g_assert_cmpstr (
      adw_action_row_get_subtitle (ADW_ACTION_ROW (find_row (GTK_WIDGET (dialog), "Firmware"))), ==,
      "Legacy BIOS");
  snapshot (window, "6-import");
  click (dialog, "Cancel");
  gtk_window_destroy (window);
}

static void
windows_requested (PirwaCreateDialog *dialog, PirwaCreate *options, guint *requests)
{
  (void)dialog;
  g_assert_cmpstr (options->source, ==, "/pirwa-ui-fixture/Windows_11.iso");
  g_assert_cmpstr (options->driver_iso, ==, "/pirwa-ui-fixture/virtio-win.iso");
  g_assert_cmpint (options->guest, ==, PIRWA_GUEST_WINDOWS_11);
  g_assert_cmpuint (options->cpus, ==, 2);
  g_assert_cmpuint (options->memory_mb, ==, 4096);
  g_assert_cmpuint (options->disk_gb, ==, 64);
  g_assert_false (options->legacy_bios);
  (*requests)++;
}

static void
test_windows (void)
{
  g_autoptr (AdwDialog) dialog = g_object_ref_sink (pirwa_create_dialog_new (NULL, TRUE));
  GtkWindow *window = present (dialog);
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Windows_11.iso", FALSE));
  click (dialog, "Continue");
  click (dialog, "Continue");
  GtkWidget *guest = find_row (GTK_WIDGET (dialog), "Operating system");
  GtkWidget *bios = find_row (GTK_WIDGET (dialog), "Use Legacy BIOS");
  adw_switch_row_set_active (ADW_SWITCH_ROW (bios), TRUE);
  adw_combo_row_set_selected (ADW_COMBO_ROW (guest), PIRWA_GUEST_WINDOWS_11);
  g_assert_false (adw_switch_row_get_active (ADW_SWITCH_ROW (bios)));
  g_assert_false (gtk_widget_is_sensitive (bios));
  g_assert_false (gtk_widget_is_sensitive (g_object_get_data (G_OBJECT (dialog), "apply")));
  GtkWidget *drivers = find_row (GTK_WIDGET (dialog), "VirtIO Driver ISO");
  g_assert_nonnull (drivers);
  g_assert_false (drop_at (drivers, "file:///pirwa-ui-fixture/driver.txt", FALSE));
  g_assert_false (drop_at (drivers, "https://example.org/driver.iso", FALSE));
  g_assert_false (drop_at (drivers, "file:///pirwa-ui-fixture/driver.iso", TRUE));
  g_assert_true (drop_at (drivers, "file:///pirwa-ui-fixture/virtio-win.iso", FALSE));
  settle ();
  snapshot (window, "7-windows-config");
  click (dialog, "Continue");
  g_assert_cmpstr (
      adw_action_row_get_subtitle (ADW_ACTION_ROW (find_row (GTK_WIDGET (dialog), "Firmware"))), ==,
      "UEFI · Secure Boot · TPM 2.0");
  g_assert_cmpstr (
      adw_action_row_get_subtitle (ADW_ACTION_ROW (find_row (GTK_WIDGET (dialog), "Driver ISO"))),
      ==, "/pirwa-ui-fixture/virtio-win.iso");
  snapshot (window, "8-windows-boot");
  click (dialog, "Config");
  adw_combo_row_set_selected (ADW_COMBO_ROW (guest), PIRWA_GUEST_WINDOWS_10);
  g_assert_true (gtk_widget_is_sensitive (bios));
  adw_combo_row_set_selected (ADW_COMBO_ROW (guest), PIRWA_GUEST_WINDOWS_11);
  click (dialog, "Continue");
  guint requests = 0;
  g_signal_connect (dialog, "create-requested", G_CALLBACK (windows_requested), &requests);
  click (dialog, "Create and Boot");
  g_assert_cmpuint (requests, ==, 1);
  click (dialog, "Cancel");
  gtk_window_destroy (window);
}

static void
test_missing_pool (void)
{
  g_autoptr (AdwDialog) dialog = g_object_ref_sink (pirwa_create_dialog_new (NULL, FALSE));
  GtkWindow *window = present (dialog);
  g_assert_true (drop (dialog, "file:///pirwa-ui-fixture/Fedora.iso", FALSE));
  click (dialog, "Continue");
  click (dialog, "Continue");
  g_assert_false (gtk_widget_is_sensitive (g_object_get_data (G_OBJECT (dialog), "apply")));
  click (dialog, "Name");
  click (dialog, "Cancel");
  gtk_window_destroy (window);
}

int
main (int argc, char **argv)
{
  gboolean dark = FALSE, large = FALSE;
  GOptionEntry entries[]
      = { { "output", 0, 0, G_OPTION_ARG_FILENAME, &output, "Screenshot directory", "PATH" },
          { "width", 0, 0, G_OPTION_ARG_INT, &width, "Window width", "PIXELS" },
          { "dark", 0, 0, G_OPTION_ARG_NONE, &dark, "Dark appearance", NULL },
          { "large-text", 0, 0, G_OPTION_ARG_NONE, &large, "Double text size", NULL },
          { NULL } };
  g_autoptr (GOptionContext) options = g_option_context_new (NULL);
  g_option_context_add_main_entries (options, entries, NULL);
  g_option_context_set_ignore_unknown_options (options, TRUE);
  g_assert_true (g_option_context_parse (options, &argc, &argv, NULL));
  g_setenv ("GTK_A11Y", "test", TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  adw_init ();
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      dark ? ADW_COLOR_SCHEME_FORCE_DARK
                                           : ADW_COLOR_SCHEME_FORCE_LIGHT);
  if (large)
    g_object_set (gtk_settings_get_default (), "gtk-font-name", "Sans 22", NULL);
  g_test_add_func ("/create/wizard", test_wizard);
  g_test_add_func ("/create/import", test_import);
  g_test_add_func ("/create/missing-pool", test_missing_pool);
  g_test_add_func ("/create/windows", test_windows);
  int result = g_test_run ();
  g_free (output);
  return result;
}
