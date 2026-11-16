/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA; all machines use the isolated libvirt test driver. */
#include "pirwa-avatar.h"
#include "pirwa-backend.h"
#include "pirwa-preferences.h"
#include "pirwa-window.h"
#include <glib/gstdio.h>

static char *output;

void __real_pirwa_backend_request_async (PirwaBackend *self, const PirwaRequest *request,
                                         GCancellable *cancellable, GAsyncReadyCallback callback,
                                         gpointer data);

void
__wrap_pirwa_backend_request_async (PirwaBackend *self, const PirwaRequest *request,
                                    GCancellable *cancellable, GAsyncReadyCallback callback,
                                    gpointer data)
{
  /* Cosmetic saves must never redefine a guest, including while it runs. */
  g_assert_cmpint (request->operation, !=, PIRWA_EDIT);
  __real_pirwa_backend_request_async (self, request, cancellable, callback, data);
}

static void
settle (void)
{
  gint64 until = g_get_monotonic_time () + 300000;
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
find_button (GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON (root) && g_strcmp0 (gtk_button_get_label (GTK_BUTTON (root)), label) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_button (child, label);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_picker (GtkWidget *root)
{
  if (g_str_equal (gtk_widget_get_name (root), "machine-avatar-picker"))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_picker (child);
      if (found)
        return found;
    }
  return NULL;
}

static guint
count_avatars (GtkWidget *root, const char *id)
{
  guint count = gtk_widget_has_css_class (root, "pirwa-avatar")
                && g_strcmp0 (pirwa_avatar_get_preset (root), id) == 0;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    count += count_avatars (child, id);
  return count;
}

static void
assert_avatar_state (GtkWidget *root, gboolean running)
{
  if (gtk_widget_has_css_class (root, "pirwa-avatar"))
    {
      g_assert_false (gtk_widget_has_css_class (root, "accent"));
      g_assert_cmpint (gtk_widget_has_css_class (root, "running"), ==, running);
      GtkWidget *screen = g_object_get_data (G_OBJECT (root), "screen");
      g_assert_cmpfloat (gtk_widget_get_opacity (screen), ==, running ? 1 : 0);
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    assert_avatar_state (child, running);
}

static GtkWidget *
find_avatar (GtkWidget *root)
{
  if (gtk_widget_has_css_class (root, "pirwa-avatar"))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *avatar = find_avatar (child);
      if (avatar)
        return avatar;
    }
  return NULL;
}

static void
snapshot (GtkWindow *window, const char *name)
{
  if (!output)
    return;
  g_assert_cmpint (g_mkdir_with_parents (output, 0700), ==, 0);
  g_autofree char *path = g_strdup_printf ("%s/%s.png", output, name);
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (window));
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot),
                          gtk_widget_get_width (GTK_WIDGET (window)),
                          gtk_widget_get_height (GTK_WIDGET (window)));
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  g_autoptr (GdkTexture) texture
      = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node, NULL);
  g_assert_true (gdk_texture_save_to_png (texture, path));
}

static GtkWindow *
open_window (GtkApplication *application, int width)
{
  GtkWindow *window = pirwa_window_new (application, "test:///default");
  gtk_window_set_default_size (window, width, 740);
  gtk_window_present (window);
  gint64 deadline = g_get_monotonic_time () + 10000000;
  while (!g_action_group_get_action_enabled (G_ACTION_GROUP (window), "edit")
         && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (window), "edit"));
  settle ();
  return window;
}

static AdwDialog *
open_settings (GtkWindow *window)
{
  g_action_group_activate_action (G_ACTION_GROUP (window), "edit", NULL);
  AdwDialog *dialog = NULL;
  gint64 deadline = g_get_monotonic_time () + 10000000;
  while (!(dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window)))
         && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_nonnull (dialog);
  settle ();
  return dialog;
}

static GtkWidget *
choose (GtkWindow *window, AdwDialog *dialog, const char *id)
{
  GtkWidget *picker = find_picker (GTK_WIDGET (dialog));
  g_assert_nonnull (picker);
  adw_expander_row_set_expanded (ADW_EXPANDER_ROW (picker), TRUE);
  settle ();
  GtkWidget *flow = find_type (picker, GTK_TYPE_FLOW_BOX);
  g_assert_nonnull (flow);
  snapshot (window, "preset-gallery");
  GtkSearchEntry *search = GTK_SEARCH_ENTRY (find_type (picker, GTK_TYPE_SEARCH_ENTRY));
  gtk_editable_set_text (GTK_EDITABLE (search), id);
  settle ();
  snapshot (window, "filtered-presets");
  for (GtkWidget *child = gtk_widget_get_first_child (flow); child;
       child = gtk_widget_get_next_sibling (child))
    if (g_strcmp0 (g_object_get_data (G_OBJECT (child), "avatar-id"), id) == 0)
      {
        g_assert_true (gtk_widget_get_child_visible (child));
        gtk_flow_box_select_child (GTK_FLOW_BOX (flow), GTK_FLOW_BOX_CHILD (child));
        g_assert_true (gtk_widget_activate (child));
        settle ();
        g_assert_cmpstr (pirwa_avatar_picker_get_preset (picker), ==, id);
        return picker;
      }
  g_assert_not_reached ();
}

static void
click (AdwDialog *dialog, const char *label)
{
  GtkWidget *button = find_button (GTK_WIDGET (dialog), label);
  g_assert_nonnull (button);
  g_assert_true (gtk_widget_is_sensitive (button));
  g_signal_emit_by_name (button, "clicked");
  settle ();
}

static void
change_state (GtkWindow *window, const char *action, const char *ready)
{
  g_action_group_activate_action (G_ACTION_GROUP (window), action, NULL);
  gint64 deadline = g_get_monotonic_time () + 10000000;
  while (!g_action_group_get_action_enabled (G_ACTION_GROUP (window), ready)
         && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (window), ready));
  settle ();
}

int
main (int argc, char **argv)
{
  int width = 1040;
  gboolean dark = FALSE, large = FALSE;
  GOptionEntry options[]
      = { { "output", 0, 0, G_OPTION_ARG_FILENAME, &output, "Screenshot directory", "DIR" },
          { "width", 0, 0, G_OPTION_ARG_INT, &width, "Window width", "PIXELS" },
          { "dark", 0, 0, G_OPTION_ARG_NONE, &dark, "Dark theme", NULL },
          { "large-text", 0, 0, G_OPTION_ARG_NONE, &large, "Double text size", NULL },
          { 0 } };
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("GTK_A11Y", "test", TRUE);
  g_autoptr (GOptionContext) context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, options, NULL);
  g_autoptr (GError) error = NULL;
  g_assert_true (g_option_context_parse (context, &argc, &argv, &error));
  g_assert_no_error (error);
  adw_init ();
  if (dark)
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        ADW_COLOR_SCHEME_FORCE_DARK);
  if (large)
    {
      g_autofree char *font = NULL;
      g_object_get (gtk_settings_get_default (), "gtk-font-name", &font, NULL);
      PangoFontDescription *description = pango_font_description_from_string (font);
      pango_font_description_set_size (description,
                                       2 * pango_font_description_get_size (description));
      g_autofree char *scaled = pango_font_description_to_string (description);
      g_object_set (gtk_settings_get_default (), "gtk-font-name", scaled, NULL);
      pango_font_description_free (description);
    }
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  g_autoptr (AdwApplication) application
      = adw_application_new ("io.github.stonega.Pirwa.AvatarQA", G_APPLICATION_NON_UNIQUE);
  g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
  GtkWindow *window = open_window (GTK_APPLICATION (application), width);
  GtkListView *list = GTK_LIST_VIEW (find_type (GTK_WIDGET (window), GTK_TYPE_LIST_VIEW));
  PirwaMachine *machine = gtk_single_selection_get_selected_item (
      GTK_SINGLE_SELECTION (gtk_list_view_get_model (list)));
  g_assert_cmpint (pirwa_machine_get_state_code (machine), ==, 1);
  g_autofree char *uuid = g_strdup (pirwa_machine_get_uuid (machine));
  g_autoptr (GSettings) settings = pirwa_preferences_new ();
  gsize count;
  const PirwaAvatarPreset *presets = pirwa_avatar_presets (&count);
  g_assert_cmpuint (count, >=, 12);
  g_assert_cmpstr (pirwa_avatar_lookup ("fedora")->id, ==, "generic");
  GtkIconTheme *theme
      = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (window)));
  for (gsize i = 0; i < count; i++)
    {
      g_autofree char *name = g_strdup_printf ("pirwa-avatar-%s-symbolic", presets[i].id);
      g_assert_true (gtk_icon_theme_has_icon (theme, name));
      g_autoptr (GtkIconPaintable) icon
          = gtk_icon_theme_lookup_icon (theme, name, NULL, 32, 1, GTK_TEXT_DIR_LTR, 0);
      g_assert_cmpint (gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (icon)), >, 0);
    }
  AdwDialog *dialog = open_settings (window);
  GtkWidget *spin = find_type (GTK_WIDGET (dialog), ADW_TYPE_SPIN_ROW);
  g_assert_false (gtk_widget_is_sensitive (spin));
  GtkWidget *picker = choose (window, dialog, "cheerful");
  snapshot (window, "running-settings");
  click (dialog, "Cancel");
  g_assert_null (pirwa_preferences_get_machine_avatar (settings, "test:///default", uuid));
  dialog = open_settings (window);
  picker = find_picker (GTK_WIDGET (dialog));
  g_assert_cmpstr (pirwa_avatar_picker_get_preset (picker), ==, "generic");
  choose (window, dialog, "cheerful");
  click (dialog, "Save Changes");
  g_autofree char *saved = pirwa_preferences_get_machine_avatar (settings, "test:///default", uuid);
  g_assert_cmpstr (saved, ==, "cheerful");
  g_assert_cmpuint (count_avatars (GTK_WIDGET (window), "cheerful"), ==, 2);
  assert_avatar_state (GTK_WIDGET (window), TRUE);
  GdkRGBA running_color, paused_color;
  gtk_widget_get_color (find_avatar (GTK_WIDGET (window)), &running_color);
  snapshot (window, "running-cheerful");
  change_state (window, "pause", "resume");
  assert_avatar_state (GTK_WIDGET (window), FALSE);
  gtk_widget_get_color (find_avatar (GTK_WIDGET (window)), &paused_color);
  g_assert_true (gdk_rgba_equal (&running_color, &paused_color));
  snapshot (window, "paused-cheerful");
  change_state (window, "resume", "pause");
  assert_avatar_state (GTK_WIDGET (window), TRUE);
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "test:///default", uuid, "missing"));
  settle ();
  g_assert_cmpuint (count_avatars (GTK_WIDGET (window), "cheerful"), ==, 0);
  g_assert_cmpuint (count_avatars (GTK_WIDGET (window), "generic"), ==, 2);
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "test:///default", uuid, "cheerful"));
  gtk_window_destroy (window);
  settle ();
  window = open_window (GTK_APPLICATION (application), width);
  g_assert_cmpuint (count_avatars (GTK_WIDGET (window), "cheerful"), ==, 2);
  change_state (window, "shutdown", "start");
  assert_avatar_state (GTK_WIDGET (window), FALSE);
  snapshot (window, "collection-cheerful");
  dialog = open_settings (window);
  choose (window, dialog, "generic");
  click (dialog, "Save Changes");
  g_assert_null (pirwa_preferences_get_machine_avatar (settings, "test:///default", uuid));
  gtk_window_destroy (window);
  settle ();
  g_print (
      "Avatar selection, cancellation, persistence, fallback and cosmetic-only saves passed.\n");
  g_free (output);
  return 0;
}
