/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA with a private test-driver VM; never starts a real guest. */
#include "pirwa-network.h"
#include "pirwa-preferences.h"
#include "pirwa-window.h"
#include <glib/gstdio.h>

static GtkWindow *window;
static GMainLoop *loop;
static char *output;
static guint step, ticks;

gboolean
__wrap_pirwa_network_start (virConnectPtr connection, virDomainPtr domain, GCancellable *cancel,
                            GError **error)
{
  (void)connection;
  (void)domain;
  (void)cancel;
  g_autofree char *xml
      = pirwa_network_domain ("<domain><devices><interface type='bridge'><source bridge='virbr0'/>"
                              "<filterref filter='clean-traffic'/></interface></devices></domain>",
                              34567, error);
  g_assert_null (xml);
  return FALSE;
}

static GtkWidget *
find_label (GtkWidget *widget, const char *prefix)
{
  if (GTK_IS_LABEL (widget) && g_str_has_prefix (gtk_label_get_text (GTK_LABEL (widget)), prefix))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_label (child, prefix);
      if (found)
        return found;
    }
  return NULL;
}

static gboolean
save_snapshot (void)
{
  if (!output)
    return TRUE;
  GtkWidget *widget = GTK_WIDGET (window);
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (widget);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot), gtk_widget_get_width (widget),
                          gtk_widget_get_height (widget));
  GskRenderNode *node = gtk_snapshot_free_to_node (snapshot);
  if (!node)
    return FALSE;
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (window));
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, NULL);
  gsk_render_node_unref (node);
  g_autofree char *path = g_build_filename (output, "start-error.png", NULL);
  g_assert_true (gdk_texture_save_to_png (texture, path));
  return TRUE;
}

static gboolean
advance (gpointer data)
{
  (void)data;
  if (++ticks > 100)
    g_error ("Network UI QA timed out at step %u", step);
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
  switch (step)
    {
    case 0:
      if (!g_action_group_get_action_enabled (G_ACTION_GROUP (window), "start"))
        break;
      g_action_group_activate_action (G_ACTION_GROUP (window), "start", NULL);
      step++;
      break;
    case 1:
    case 3:
      if (!dialog || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      {
        g_assert_true (ADW_IS_ALERT_DIALOG (dialog));
        AdwAlertDialog *alert = ADW_ALERT_DIALOG (dialog);
        g_assert_cmpstr (adw_alert_dialog_get_heading (alert), ==,
                         "Could Not Start Virtual Machine");
        const char *message = adw_alert_dialog_get_body (alert);
        g_assert_nonnull (strstr (message, "Preferences → Network."));
        g_assert_cmpstr (adw_alert_dialog_get_default_response (alert), ==, "close");
        g_assert_cmpstr (adw_alert_dialog_get_close_response (alert), ==, "close");
        GtkWidget *body = find_label (GTK_WIDGET (dialog), message);
        g_assert_nonnull (body);
        g_assert_true (gtk_label_get_wrap (GTK_LABEL (body)));
        g_assert_false (pango_layout_is_ellipsized (gtk_label_get_layout (GTK_LABEL (body))));
        if (step == 1 && !save_snapshot ())
          break;
        if (step == 1)
          {
            GtkWidget *label = find_label (GTK_WIDGET (dialog), "Preferences");
            g_assert_nonnull (label);
            GtkWidget *button = gtk_widget_get_ancestor (label, GTK_TYPE_BUTTON);
            g_assert_nonnull (button);
            g_assert_true (gtk_widget_activate (button));
          }
        else
          adw_dialog_close (dialog);
        step++;
      }
      break;
    case 2:
      if (!dialog || !ADW_IS_PREFERENCES_DIALOG (dialog)
          || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      g_assert_nonnull (find_label (GTK_WIDGET (dialog), "Use host proxy"));
      adw_dialog_force_close (dialog);
      g_action_group_activate_action (G_ACTION_GROUP (window), "start", NULL);
      step++;
      break;
    case 4:
      if (dialog)
        break;
      {
        g_autoptr (GSettings) settings = pirwa_preferences_new ();
        g_assert_true (g_settings_get_boolean (settings, "use-host-proxy"));
        g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (window), "start"));
        gtk_window_destroy (window);
        g_print ("PASS wrapped start error, full recovery text, Preferences response, "
                 "retry and dismissal without changing proxy settings\n");
        g_main_loop_quit (loop);
        return G_SOURCE_REMOVE;
      }
    }
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  gboolean dark = FALSE, large_text = FALSE;
  int width = 1040;
  GOptionEntry entries[]
      = { { "output", 0, 0, G_OPTION_ARG_FILENAME, &output, "Screenshot directory", "DIR" },
          { "dark", 0, 0, G_OPTION_ARG_NONE, &dark, "Dark theme", NULL },
          { "large-text", 0, 0, G_OPTION_ARG_NONE, &large_text, "200% text", NULL },
          { "width", 0, 0, G_OPTION_ARG_INT, &width, "Window width", "PIXELS" },
          { NULL } };
  g_autoptr (GOptionContext) options = g_option_context_new (NULL);
  g_option_context_add_main_entries (options, entries, NULL);
  g_assert_true (g_option_context_parse (options, &argc, &argv, NULL));
  g_setenv ("GTK_A11Y", "test", TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  adw_init ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  if (large_text)
    g_object_set (gtk_settings_get_default (), "gtk-xft-dpi", 192 * 1024, NULL);
  if (dark)
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        ADW_COLOR_SCHEME_FORCE_DARK);
  if (output)
    g_assert_cmpint (g_mkdir_with_parents (output, 0700), ==, 0);
  g_autofree char *directory = g_dir_make_tmp ("pirwa-network-ui-XXXXXX", NULL);
  g_autofree char *node = g_build_filename (directory, "node.xml", NULL);
  g_assert_true (g_file_set_contents (
      node,
      "<node><domain type='test'><name>Network Test</name><memory>524288</memory>"
      "<vcpu>1</vcpu><os><type>hvm</type></os>"
      "<test:runstate xmlns:test='http://libvirt.org/schemas/domain/test/1.0'>5</test:runstate>"
      "</domain></node>",
      -1, NULL));
  g_autofree char *uri = g_strconcat ("test://", node, NULL);
  g_autoptr (GtkApplication) application
      = gtk_application_new ("io.github.stonega.NetworkQA", G_APPLICATION_NON_UNIQUE);
  g_assert_true (g_application_register (G_APPLICATION (application), NULL, NULL));
  loop = g_main_loop_new (NULL, FALSE);
  window = pirwa_window_new (application, uri);
  gtk_window_set_default_size (window, width, 680);
  gtk_window_present (window);
  g_timeout_add (100, advance, NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  g_unlink (node);
  g_rmdir (directory);
  g_free (output);
  return 0;
}
