/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA with synthetic domains; never opens a real guest. */
#include "pirwa-console.h"
#include "pirwa-display.h"
#include "pirwa-window.h"
#include <glib/gstdio.h>

static GtkApplication *application;
static GtkWindow *window;
static PirwaConsole *console;
static GtkSingleSelection *selection;
static GMainLoop *loop;
static char *output;
static guint step;
static guint ticks;
static int width = 1040;
static GDBusConnection *desktop_bus;
static char *keyboard_path;
static guint focus_source;
static gint64 focus_requested_at;

static GVariant *
keyboard_call (const char *method, GVariant *parameters)
{
  g_autoptr (GError) error = NULL;
  GVariant *reply
      = g_dbus_connection_call_sync (desktop_bus, "org.gnome.Mutter.RemoteDesktop", keyboard_path,
                                     "org.gnome.Mutter.RemoteDesktop.Session", method, parameters,
                                     NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reply);
  return reply;
}

static void
setup_private_keyboard (void)
{
  if (!g_getenv ("PIRWA_QA_ISOLATED"))
    return;
  g_autofree char *parent = g_path_get_dirname (g_get_user_runtime_dir ());
  g_autofree char *name = g_path_get_basename (parent);
  g_assert_true (g_str_has_prefix (name, "pirwa-gnome-"));
  desktop_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull (desktop_bus);
  g_autoptr (GVariant) created = g_dbus_connection_call_sync (
      desktop_bus, "org.gnome.Mutter.RemoteDesktop", "/org/gnome/Mutter/RemoteDesktop",
      "org.gnome.Mutter.RemoteDesktop", "CreateSession", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
      NULL, NULL);
  g_assert_nonnull (created);
  g_variant_get (created, "(o)", &keyboard_path);
  g_autoptr (GVariant) started = keyboard_call ("Start", NULL);
}

static gboolean
focus_private_window (gpointer data)
{
  (void)data;
  /* A headless compositor has no physical keyboard seat. Activate the mapped
   * window through normal Alt+Tab input when the compositor has suspended it.
   * Separate events let the compositor dispatch modifier state between keys. */
  const guint keys[] = { 56, 15, 15, 56 };
  static guint i;
  g_autoptr (GVariant) reply
      = keyboard_call ("NotifyKeyboardKeycode", g_variant_new ("(ub)", keys[i], i < 2));
  if (++i < G_N_ELEMENTS (keys))
    return G_SOURCE_CONTINUE;
  i = 0;
  focus_source = 0;
  return G_SOURCE_REMOVE;
}

static GtkWidget *
find_type (GtkWidget *widget, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_type (child, type);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_action (GtkWidget *widget, const char *action)
{
  if (GTK_IS_ACTIONABLE (widget)
      && g_strcmp0 (gtk_actionable_get_action_name (GTK_ACTIONABLE (widget)), action) == 0)
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_action (child, action);
      if (found)
        return found;
    }
  return NULL;
}

static void
activate (const char *action)
{
  g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (window), action));
  g_action_group_activate_action (G_ACTION_GROUP (window), action, NULL);
}

static const char *
widget_text (GtkWidget *widget)
{
  if (GTK_IS_LABEL (widget))
    return gtk_label_get_text (GTK_LABEL (widget));
  if (GTK_IS_INSCRIPTION (widget))
    return gtk_inscription_get_text (GTK_INSCRIPTION (widget));
  return NULL;
}

static GtkWidget *
find_label_prefix (GtkWidget *widget, const char *prefix)
{
  const char *text = widget_text (widget);
  if (text && g_str_has_prefix (text, prefix))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_label_prefix (child, prefix);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWindow *
popup (void)
{
  for (GList *item = gtk_application_get_windows (application); item; item = item->next)
    if (item->data != window)
      return item->data;
  return NULL;
}

static gboolean
save_snapshot (GtkWindow *target, const char *name)
{
  if (!output)
    return TRUE;
  GtkWidget *widget = GTK_WIDGET (target);
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (widget);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot), gtk_widget_get_width (widget),
                          gtk_widget_get_height (widget));
  GskRenderNode *node = gtk_snapshot_free_to_node (snapshot);
  if (!node)
    return FALSE;
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (target));
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, NULL);
  gsk_render_node_unref (node);
  g_autofree char *path = g_build_filename (output, name, NULL);
  g_assert_true (gdk_texture_save_to_png (texture, path));
  return TRUE;
}

static void
assert_header_action (const char *name)
{
  GtkWidget *button = find_action (GTK_WIDGET (window), name);
  g_assert_nonnull (button);
  GtkWidget *header = gtk_widget_get_ancestor (button, ADW_TYPE_HEADER_BAR);
  g_assert_nonnull (header);
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (button, GTK_WIDGET (window), &bounds));
  g_assert_cmpfloat (bounds.origin.x, >=, 0);
  g_assert_cmpfloat (bounds.origin.x + bounds.size.width, <=,
                     gtk_widget_get_width (GTK_WIDGET (window)));
}

static gboolean
advance (gpointer data)
{
  (void)data;
  if (++ticks > 150)
    {
      GtkWidget *reading = find_label_prefix (GTK_WIDGET (window), "CPU ");
      if (!reading)
        reading = find_label_prefix (GTK_WIDGET (window), "Sampling usage");
      if (!reading)
        reading = find_label_prefix (GTK_WIDGET (window), "Usage unavailable");
      g_error ("Console UI QA timed out at step %u (active=%d, mapped=%d, usage=%s)", step,
               gtk_window_is_active (window), gtk_widget_get_mapped (GTK_WIDGET (window)),
               reading ? widget_text (reading) : "none");
    }
  switch (step)
    {
    case 0:
      if (!g_action_group_get_action_enabled (G_ACTION_GROUP (window), "new"))
        break;
      {
        GtkWidget *list = find_type (GTK_WIDGET (window), GTK_TYPE_LIST_VIEW);
        g_assert_nonnull (list);
        selection = GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (list)));
        gtk_single_selection_set_selected (selection, 0);
        step++;
      }
      break;
    case 1:
      console = (PirwaConsole *)find_type (GTK_WIDGET (window), PIRWA_TYPE_CONSOLE);
      if (!console)
        break;
      if (desktop_bus && gtk_window_is_suspended (window))
        {
          gint64 now = g_get_monotonic_time ();
          if (!focus_source && now - focus_requested_at >= G_USEC_PER_SEC)
            {
              /* Initial mapping/overview transitions can consume a request.
               * Retry only until visible; the existing watchdog bounds QA. */
              focus_requested_at = now;
              focus_source = g_timeout_add (40, focus_private_window, NULL);
            }
          break;
        }
      GtkWidget *usage = find_label_prefix (GTK_WIDGET (window), "CPU ");
      if (!usage)
        usage = find_label_prefix (GTK_WIDGET (window), "Usage unavailable");
      if (!usage && gtk_window_is_suspended (window))
        usage = find_label_prefix (GTK_WIDGET (window), "Sampling usage");
      if (!usage)
        break; /* A visible window must complete a real backend sample. */
      GtkWidget *host = find_label_prefix (GTK_WIDGET (window), "x86_64 ·");
      g_assert_nonnull (host);
      GtkWidget *network = find_label_prefix (GTK_WIDGET (window), "↓ ");
      if (!network)
        network = find_label_prefix (GTK_WIDGET (window), "NET ");
      g_assert_nonnull (network);
      g_assert_true (gtk_widget_is_visible (network));
      GtkPaned *paned = GTK_PANED (find_type (GTK_WIDGET (window), GTK_TYPE_PANED));
      GtkWidget *sidebar = gtk_paned_get_start_child (paned);
      g_assert_cmpint (gtk_widget_get_height (sidebar), ==,
                       gtk_widget_get_height (GTK_WIDGET (window)));
      graphene_rect_t host_bounds, usage_bounds;
      g_assert_true (gtk_widget_compute_bounds (host, GTK_WIDGET (window), &host_bounds));
      g_assert_true (gtk_widget_compute_bounds (usage, GTK_WIDGET (window), &usage_bounds));
      g_assert_cmpfloat (host_bounds.origin.x, >=, gtk_widget_get_width (sidebar));
      g_assert_cmpfloat (host_bounds.origin.x, <, usage_bounds.origin.x);
      g_assert_cmpfloat (usage_bounds.origin.x + usage_bounds.size.width, <=,
                         gtk_widget_get_width (GTK_WIDGET (window)));
      g_assert_cmpfloat (gtk_inscription_get_xalign (GTK_INSCRIPTION (host)), ==, 0);
      g_assert_cmpfloat (gtk_inscription_get_xalign (GTK_INSCRIPTION (usage)), ==, 1);
      GtkWidget *header = gtk_widget_get_ancestor (host, ADW_TYPE_HEADER_BAR);
      g_assert_nonnull (header);
      g_assert_true (gtk_widget_get_ancestor (usage, ADW_TYPE_HEADER_BAR) == header);
      g_assert_true (gtk_widget_get_ancestor (network, ADW_TYPE_HEADER_BAR) == header);
      g_assert_cmpint (adw_toolbar_view_get_bottom_bar_height (
                           ADW_TOOLBAR_VIEW (gtk_paned_get_end_child (paned))),
                       ==, 0);
      g_assert_cmpint (gtk_widget_get_width (GTK_WIDGET (window)), <=, width);
      assert_header_action ("win.pause");
      assert_header_action ("win.shutdown");
      assert_header_action ("win.reboot");
      assert_header_action ("win.pop-out");
      g_assert_null (find_action (GTK_WIDGET (window), "win.edit"));
      g_assert_null (find_action (GTK_WIDGET (window), "win.force-stop"));
      g_assert_null (find_action (GTK_WIDGET (window), "win.remove"));
      if (!save_snapshot (window, "embedded.png"))
        break;
      g_object_add_weak_pointer (G_OBJECT (console), (gpointer *)&console);
      activate ("pop-out");
      g_assert_true (pirwa_console_is_detached (console));
      step++;
      break;
    case 2:
      {
        g_assert_nonnull (popup ());
        g_assert_true (gtk_widget_get_root (GTK_WIDGET (console)) == GTK_ROOT (popup ()));
        GtkWidget *bring_back = find_action (GTK_WIDGET (window), "win.return-viewer");
        g_assert_nonnull (bring_back);
        g_assert_true (gtk_widget_get_mapped (bring_back));
        graphene_rect_t bounds;
        g_assert_true (gtk_widget_compute_bounds (bring_back, GTK_WIDGET (window), &bounds));
        g_assert_cmpfloat (bounds.origin.y, <, 140);
        if (!save_snapshot (window, "detached.png") || !save_snapshot (popup (), "popup.png"))
          break;
        activate ("return-viewer");
        g_assert_null (popup ());
        g_assert_true (gtk_widget_get_root (GTK_WIDGET (console)) == GTK_ROOT (window));
        g_assert_false (pirwa_console_is_detached (console));
        activate ("pop-out");
        gtk_window_fullscreen (popup ());
        step++;
      }
      break;
    case 3:
      if (!gtk_window_is_fullscreen (popup ()))
        break;
      {
        AdwToolbarView *toolbar
            = ADW_TOOLBAR_VIEW (find_type (GTK_WIDGET (console), ADW_TYPE_TOOLBAR_VIEW));
        g_assert_false (adw_toolbar_view_get_reveal_top_bars (toolbar));
        g_assert_cmpint (adw_toolbar_view_get_bottom_bar_height (toolbar), ==, 0);
        if (!save_snapshot (popup (), "fullscreen.png"))
          break;
        gtk_window_close (popup ()); /* Closing the selected popup docks it. */
        g_assert_null (popup ());
        g_assert_true (gtk_widget_get_root (GTK_WIDGET (console)) == GTK_ROOT (window));
        activate ("pop-out");
        gtk_single_selection_set_selected (selection, 1);
        step++;
      }
      break;
    case 4:
      {
        GtkWidget *other = find_type (GTK_WIDGET (window), PIRWA_TYPE_CONSOLE);
        if (!other)
          break;
        g_assert_true (other != GTK_WIDGET (console));
        g_assert_nonnull (popup ());
        gtk_single_selection_set_selected (selection, 0);
        step++;
      }
      break;
    case 5:
      if (!g_action_group_get_action_enabled (G_ACTION_GROUP (window), "return-viewer"))
        break;
      activate ("return-viewer");
      g_assert_true (find_type (GTK_WIDGET (window), PIRWA_TYPE_CONSOLE) == GTK_WIDGET (console));
      activate ("pop-out");
      gtk_window_close (window); /* An explicit popup survives its collection. */
      window = NULL;
      g_assert_nonnull (console);
      g_assert_nonnull (popup ());
      step++;
      break;
    case 6:
      gtk_window_close (popup ());
      step++;
      break;
    case 7:
      if (console)
        break;
      g_print ("PASS embedded viewer, header actions, pop-out/return identity, fullscreen, "
               "selection changes, popup close, collection close, final session disposal\n");
      g_main_loop_quit (loop);
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  gboolean dark = FALSE, large_text = FALSE;
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
  adw_init ();
  setup_private_keyboard ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  if (large_text)
    g_object_set (gtk_settings_get_default (), "gtk-xft-dpi", 192 * 1024, NULL);
  if (dark)
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        ADW_COLOR_SCHEME_FORCE_DARK);
  if (output)
    g_assert_cmpint (g_mkdir_with_parents (output, 0700), ==, 0);
  g_autofree char *directory = g_dir_make_tmp ("pirwa-console-ui-XXXXXX", NULL);
  g_autofree char *node = g_build_filename (directory, "node.xml", NULL);
  const char *xml
      = "<node><cpu><model>x86_64</model><mhz>2400</mhz><active>4</active><nodes>1</nodes>"
        "<sockets>1</sockets><cores>4</cores><threads>1</threads></cpu><memory>8388608</memory>"
        "<domain type='test'><name>Embedded "
        "Test</name><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<memory>1048576</memory><vcpu>2</vcpu><os><type>hvm</type></os>"
        "<devices><graphics type='spice' autoport='yes'/></devices></domain>"
        "<domain type='test'><name>Other "
        "Test</name><uuid>22222222-2222-4222-8222-222222222222</uuid>"
        "<memory>1048576</memory><vcpu>2</vcpu><os><type>hvm</type></os>"
        "<devices><graphics type='spice' autoport='yes'/></devices></domain></node>";
  g_assert_true (g_file_set_contents (node, xml, -1, NULL));
  g_autofree char *uri = g_strconcat ("test://", node, NULL);
  application = gtk_application_new ("io.github.pirwa.ConsoleQA", G_APPLICATION_NON_UNIQUE);
  g_assert_true (g_application_register (G_APPLICATION (application), NULL, NULL));
  loop = g_main_loop_new (NULL, FALSE);
  window = pirwa_window_new (application, uri);
  gtk_window_set_default_size (window, width, 680);
  gtk_window_present (window);
  g_timeout_add (200, advance, NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  if (desktop_bus)
    {
      if (focus_source)
        g_source_remove (focus_source);
      g_autoptr (GVariant) stopped = keyboard_call ("Stop", NULL);
      g_clear_pointer (&keyboard_path, g_free);
      g_clear_object (&desktop_bus);
    }
  g_object_unref (application);
  g_unlink (node);
  g_rmdir (directory);
  g_free (output);
  return 0;
}
