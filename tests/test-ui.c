/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA, never run by the default headless test suite. */
#include "pirwa-machine.h"
#include "pirwa-window.h"
#include <glib/gstdio.h>
#include <locale.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
  GtkApplication *application;
  GtkWindow *window;
  GdkFrameClock *clock;
  gulong paint_handler;
  gulong before_handler;
  gint64 before_paint;
  GArray *frame_costs;
  char *renderer;
  char *focus_kind;
  gboolean window_active;
  int refresh_millihz;
  int surface_scale;
  char *initial_focus_kind;
  guint domains;
  gboolean scrolling;
  gboolean sampling;
  gint64 start;
  gint64 first_paint;
  gint64 ready;
  gint64 idle_start;
  gint64 idle_cpu_start;
  gint64 idle_cpu_ns;
  gint64 idle_wall_us;
  gint64 frame_count;
  gint64 idle_frames;
  gint64 pss_kib;
  int duration;
  int requested_width;
  int step;
  guint watch;
  guint timeout;
  gboolean exercise;
  gboolean finished;
  const char *snapshot;
  PirwaMachine *original;
  gboolean boxes_exercise;
  PirwaBoxesPaths *boxes_paths;
  char *boxes_root;
  int boxes_socket;
} Run;

static gint64
cpu_time (void)
{
  struct timespec time;
  clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &time);
  return (gint64)time.tv_sec * 1000000000 + time.tv_nsec;
}

static gint64
pss (void)
{
  g_autofree char *contents = NULL;
  if (!g_file_get_contents ("/proc/self/smaps_rollup", &contents, NULL, NULL))
    return -1;
  char *line = strstr (contents, "\nPss:");
  return line ? g_ascii_strtoll (line + 5, NULL, 10) : -1;
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
find_machine_list (GtkWidget *root)
{
  if (GTK_IS_LIST_VIEW (root))
    {
      GListModel *model = G_LIST_MODEL (gtk_list_view_get_model (GTK_LIST_VIEW (root)));
      if (GTK_IS_SINGLE_SELECTION (model))
        model = gtk_single_selection_get_model (GTK_SINGLE_SELECTION (model));
      while (GTK_IS_FILTER_LIST_MODEL (model))
        model = gtk_filter_list_model_get_model (GTK_FILTER_LIST_MODEL (model));
      if (model && g_list_model_get_item_type (model) == PIRWA_TYPE_MACHINE)
        return root;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_machine_list (child);
      if (found)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_button (GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON (root)
      && (g_strcmp0 (gtk_button_get_label (GTK_BUTTON (root)), label) == 0
          || g_strcmp0 (gtk_widget_get_tooltip_text (root), label) == 0))
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
find_action (GtkWidget *root, const char *action)
{
  if (gtk_widget_get_mapped (root) && GTK_IS_BUTTON (root)
      && gtk_button_get_icon_name (GTK_BUTTON (root)) && GTK_IS_ACTIONABLE (root)
      && g_strcmp0 (gtk_actionable_get_action_name (GTK_ACTIONABLE (root)), action) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_action (child, action);
      if (found)
        return found;
    }
  return NULL;
}

static gboolean
has_label (GtkWidget *root, const char *text)
{
  if (GTK_IS_LABEL (root) && g_strcmp0 (gtk_label_get_text (GTK_LABEL (root)), text) == 0)
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    if (has_label (child, text))
      return TRUE;
  return FALSE;
}

static GtkMenuButton *
find_machine_menu (GtkWidget *root, const char *uuid)
{
  if (GTK_IS_MENU_BUTTON (root))
    {
      GMenuModel *model = gtk_menu_button_get_menu_model (GTK_MENU_BUTTON (root));
      if (model && g_menu_model_get_n_items (model) > 0)
        {
          g_autoptr (GVariant) target = g_menu_model_get_item_attribute_value (
              model, 0, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
          if (target && g_str_equal (g_variant_get_string (target, NULL), uuid))
            return GTK_MENU_BUTTON (root);
        }
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkMenuButton *found = find_machine_menu (child, uuid);
      if (found)
        return found;
    }
  return NULL;
}

static void
activate_machine_menu (Run *run, const char *action)
{
  const char *uuid = pirwa_machine_get_uuid (run->original);
  GtkMenuButton *button = find_machine_menu (GTK_WIDGET (run->window), uuid);
  g_assert_nonnull (button);
  gtk_menu_button_popup (button);
  GMenuModel *model = gtk_menu_button_get_menu_model (button);
  for (int i = 0; i < g_menu_model_get_n_items (model); i++)
    {
      g_autofree char *name = NULL;
      g_menu_model_get_item_attribute (model, i, G_MENU_ATTRIBUTE_ACTION, "s", &name);
      if (g_strcmp0 (name, action) != 0)
        continue;
      g_autoptr (GVariant) target = g_menu_model_get_item_attribute_value (
          model, i, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
      g_assert_cmpstr (g_variant_get_string (target, NULL), ==, uuid);
      g_assert_true (g_action_group_get_action_enabled (G_ACTION_GROUP (run->window), name + 4));
      gtk_menu_button_popdown (button);
      g_assert_true (gtk_widget_activate_action_variant (GTK_WIDGET (button), name, target));
      return;
    }
  g_error ("Machine menu action was not found: %s", action);
}

static gboolean
enabled (Run *run, const char *action)
{
  return g_action_group_get_action_enabled (G_ACTION_GROUP (run->window), action);
}

static void
activate (Run *run, const char *action)
{
  g_assert_true (enabled (run, action));
  g_action_group_activate_action (G_ACTION_GROUP (run->window), action, NULL);
}

static void
save_snapshot (Run *run)
{
  if (!run->snapshot)
    return;
  int width = gtk_widget_get_width (GTK_WIDGET (run->window));
  int height = gtk_widget_get_height (GTK_WIDGET (run->window));
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (run->window));
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot), width, height);
  GskRenderNode *node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (run->window));
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, NULL);
  gsk_render_node_unref (node);
  g_assert_nonnull (texture);
  g_assert_true (gdk_texture_save_to_png (texture, run->snapshot));
}

static void
finish (Run *run)
{
  run->finished = TRUE;
  if (!run->exercise)
    save_snapshot (run);
  if (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (run->window)))
    gtk_window_destroy (run->window); /* Profiling may end with onboarding open. */
  else
    gtk_window_close (run->window);
}

static gboolean
sample_end (gpointer data)
{
  Run *run = data;
  run->sampling = FALSE;
  run->idle_wall_us = g_get_monotonic_time () - run->idle_start;
  run->idle_cpu_ns = cpu_time () - run->idle_cpu_start;
  run->idle_frames = run->frame_count - run->idle_frames;
  run->pss_kib = pss ();
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (run->window));
  run->focus_kind = g_strdup (focus ? G_OBJECT_TYPE_NAME (focus) : "none");
  run->window_active = gtk_window_is_active (run->window);
  finish (run);
  return G_SOURCE_REMOVE;
}

static gboolean
sample_start (gpointer data)
{
  Run *run = data;
  run->sampling = TRUE;
  run->idle_start = g_get_monotonic_time ();
  run->idle_cpu_start = cpu_time ();
  run->idle_frames = run->frame_count;
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (run->window));
  run->initial_focus_kind = g_strdup (focus ? G_OBJECT_TYPE_NAME (focus) : "none");
  g_timeout_add_seconds (run->duration, sample_end, run);
  return G_SOURCE_REMOVE;
}

static void
after_paint (GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  Run *run = data;
  run->frame_count++;
  if (run->sampling && run->scrolling && run->before_paint)
    {
      gint64 cost = g_get_monotonic_time () - run->before_paint;
      g_array_append_val (run->frame_costs, cost);
    }
  if (!run->first_paint)
    run->first_paint = g_get_monotonic_time ();
}

static void
before_paint (GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  ((Run *)data)->before_paint = g_get_monotonic_time ();
}

static gboolean
scroll_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  Run *run = data;
  if (run->sampling)
    {
      GtkWidget *parent = gtk_widget_get_parent (widget);
      while (parent && !GTK_IS_SCROLLED_WINDOW (parent))
        parent = gtk_widget_get_parent (parent);
      g_assert_nonnull (parent);
      GtkAdjustment *adjustment
          = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (parent));
      double limit
          = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment);
      g_assert_cmpfloat (limit, >, 0);
      double next = gtk_adjustment_get_value (adjustment) + 40;
      gtk_adjustment_set_value (adjustment, next > limit ? 0 : next);
    }
  return G_SOURCE_CONTINUE;
}

static void
mapped (GtkWidget *widget, gpointer data)
{
  Run *run = data;
  run->clock = g_object_ref (gtk_widget_get_frame_clock (widget));
  run->renderer = g_strdup (G_OBJECT_TYPE_NAME (gtk_native_get_renderer (GTK_NATIVE (widget))));
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  GdkMonitor *monitor
      = gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface);
  run->refresh_millihz = monitor ? gdk_monitor_get_refresh_rate (monitor) : 0;
  run->surface_scale = gdk_surface_get_scale_factor (surface);
  run->before_handler
      = g_signal_connect (run->clock, "before-paint", G_CALLBACK (before_paint), run);
  run->paint_handler = g_signal_connect (run->clock, "after-paint", G_CALLBACK (after_paint), run);
}

static void
assert_visible_button (GtkWidget *dialog, GtkWidget *button)
{
  g_assert_nonnull (button);
  g_assert_true (gtk_widget_get_mapped (button));
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (button, dialog, &bounds));
  g_assert_cmpfloat (bounds.origin.x, >=, 0);
  g_assert_cmpfloat (bounds.origin.y, >=, 0);
  g_assert_cmpfloat (bounds.origin.x + bounds.size.width, <=, gtk_widget_get_width (dialog) + 1);
  g_assert_cmpfloat (bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height (dialog) + 1);
  GtkWidget *label = find_type (button, GTK_TYPE_LABEL);
  g_assert_nonnull (label);
  g_assert_false (pango_layout_is_ellipsized (gtk_label_get_layout (GTK_LABEL (label))));
}

static gboolean
watch_boxes (gpointer data)
{
  Run *run = data;
  AdwDialog *dialog
      = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (run->window));
  GtkWidget *import = dialog ? find_button (GTK_WIDGET (dialog), "Import Machine") : NULL;
  switch (run->step)
    {
    case 0:
      if (!dialog || !import || !gtk_widget_get_mapped (import) || !run->first_paint)
        break;
      run->ready = g_get_monotonic_time ();
      g_assert_cmpstr (adw_dialog_get_title (dialog), ==, "Import from Boxes Flatpak");
      assert_visible_button (GTK_WIDGET (dialog), import);
      g_assert_false (gtk_widget_get_sensitive (import));
      GtkWidget *view = find_machine_list (GTK_WIDGET (dialog));
      GtkSingleSelection *selection
          = GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (view)));
      g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (selection)), ==, 1);
      PirwaMachine *machine = gtk_single_selection_get_selected_item (selection);
      g_assert_cmpstr (pirwa_machine_get_name (machine), ==, "Flatpak Example");
      g_assert_null (find_type (GTK_WIDGET (dialog), GTK_TYPE_DROP_DOWN));
      run->domains = 1;
      save_snapshot (run);
      g_assert_true (
          gtk_widget_get_visible (find_button (GTK_WIDGET (dialog), "Open GNOME Boxes")));
      g_signal_emit_by_name (find_button (GTK_WIDGET (dialog), "Refresh Library"), "clicked");
      run->step++;
      break;
    case 1:
      if (!dialog || !import || !gtk_widget_get_mapped (import))
        break;
      g_assert_false (gtk_widget_get_sensitive (import));
      if (run->requested_width == 720)
        gtk_window_close (run->window);
      else
        g_signal_emit_by_name (find_button (GTK_WIDGET (dialog), "Not Now"), "clicked");
      run->step++;
      break;
    case 2:
      if (dialog || !enabled (run, "import-boxes"))
        break;
      g_assert_true (g_file_test (run->boxes_paths->state_file, G_FILE_TEST_EXISTS));
      GtkWindow *previous = run->window;
      run->window = pirwa_window_new_with_boxes (run->application, run->boxes_paths->native_uri,
                                                 run->boxes_paths);
      gtk_window_set_default_size (run->window, run->requested_width, 680);
      gtk_window_present (run->window);
      gtk_window_close (previous);
      run->step++;
      break;
    case 3:
      {
        if (!enabled (run, "new") || !enabled (run, "import-boxes"))
          break;
        g_assert_null (dialog);
        g_autofree char *directory
            = g_build_filename (run->boxes_paths->runtime_root, "libvirt", NULL);
        g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
        g_autofree char *socket_path = g_build_filename (directory, "libvirt-sock", NULL);
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        g_assert_cmpuint (g_strlcpy (address.sun_path, socket_path, sizeof address.sun_path), <,
                          sizeof address.sun_path);
        run->boxes_socket = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        g_assert_cmpint (run->boxes_socket, >=, 0);
        /* An owned but non-listening socket makes discovery available; connection
         * then fails immediately, exercising error/retry without any real service. */
        g_assert_cmpint (bind (run->boxes_socket, (struct sockaddr *)&address, sizeof address), ==,
                         0);
        activate (run, "import-boxes");
        run->step++;
        break;
      }
    case 4:
      if (!dialog || !import || !gtk_widget_get_mapped (import))
        break;
      g_assert_true (gtk_widget_get_sensitive (import));
      g_signal_emit_by_name (import, "clicked");
      run->step++;
      break;
    case 5:
      if (!dialog || !adw_dialog_get_can_close (dialog) || !gtk_widget_get_sensitive (import))
        break;
      GtkLabel *message = g_object_get_data (G_OBJECT (dialog), "error");
      if (!*gtk_label_get_text (message))
        break;
      g_assert_true (gtk_widget_get_visible (GTK_WIDGET (message)));
      g_assert_nonnull (find_button (GTK_WIDGET (dialog), "Not Now"));
      g_signal_emit_by_name (find_button (GTK_WIDGET (dialog), "Not Now"), "clicked");
      run->step++;
      break;
    case 6:
      if (dialog || !enabled (run, "import-boxes"))
        break;
      /* The native VM was available throughout, without importing anything. */
      GtkWidget *list = find_machine_list (GTK_WIDGET (run->window));
      GtkSingleSelection *model
          = GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (list)));
      g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 1);
      gtk_single_selection_set_selected (model, 0);
      run->step++;
      break;
    case 7:
      if (!enabled (run, "pause"))
        break;
      /* A fresh first-run state plus residual definitions must remain quiet
       * after the Flatpak has been uninstalled. */
      g_assert_true (
          g_file_set_contents (run->boxes_paths->flatpak_program, "#!/bin/sh\nexit 1\n", -1, NULL));
      g_assert_cmpint (g_chmod (run->boxes_paths->flatpak_program, 0700), ==, 0);
      g_assert_cmpint (g_unlink (run->boxes_paths->state_file), ==, 0);
      GtkWindow *old = run->window;
      run->window = pirwa_window_new_with_boxes (run->application, run->boxes_paths->native_uri,
                                                 run->boxes_paths);
      gtk_window_present (run->window);
      gtk_window_close (old);
      run->step++;
      break;
    case 8:
      if (!enabled (run, "new") || !enabled (run, "import-boxes"))
        break;
      g_assert_null (dialog);
      g_assert_false (g_file_test (run->boxes_paths->state_file, G_FILE_TEST_EXISTS));
      run->watch = 0;
      finish (run);
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
watch (gpointer data)
{
  Run *run = data;
  if (run->boxes_exercise)
    return watch_boxes (data);
  AdwDialog *dialog
      = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (run->window));
  if (!run->ready)
    {
      if (!enabled (run, "new") || !run->first_paint)
        return G_SOURCE_CONTINUE;
      run->ready = g_get_monotonic_time ();
      GtkWidget *spinner = find_type (GTK_WIDGET (run->window), GTK_TYPE_SPINNER);
      g_assert_nonnull (spinner);
      g_assert_false (gtk_spinner_get_spinning (GTK_SPINNER (spinner)));
      if (run->exercise)
        g_assert_cmpint (gtk_widget_get_width (GTK_WIDGET (run->window)), <=, run->requested_width);
      GtkWidget *list = find_machine_list (GTK_WIDGET (run->window));
      g_assert_nonnull (list);
      run->domains = g_list_model_get_n_items (
          G_LIST_MODEL (gtk_list_view_get_model (GTK_LIST_VIEW (list))));
      if (run->scrolling)
        gtk_widget_add_tick_callback (list, scroll_tick, run, NULL);
      if (!run->exercise)
        {
          run->watch = 0;
          if (run->duration > 0)
            g_timeout_add_seconds (2, sample_start, run);
          else
            {
              run->pss_kib = pss ();
              finish (run);
            }
          return G_SOURCE_REMOVE;
        }
    }
  GtkWidget *view = find_machine_list (GTK_WIDGET (run->window));
  g_assert_nonnull (view);
  GtkSingleSelection *selection
      = GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (view)));
  switch (run->step)
    {
    case 0:
      gtk_test_accessible_assert_property (find_action (GTK_WIDGET (run->window), "win.new"),
                                           GTK_ACCESSIBLE_PROPERTY_LABEL, "New Virtual Machine");
      GtkWidget *search_button = find_button (GTK_WIDGET (run->window), "Search Virtual Machines");
      g_assert_nonnull (search_button);
      gtk_test_accessible_assert_property (search_button, GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           "Search Virtual Machines");
      GtkSearchBar *search_bar
          = GTK_SEARCH_BAR (find_type (GTK_WIDGET (run->window), GTK_TYPE_SEARCH_BAR));
      g_assert_nonnull (search_bar);
      g_assert_false (gtk_search_bar_get_search_mode (search_bar));
      g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (search_button)));
      if (run->domains == 0)
        {
          activate (run, "new");
          run->step = 11;
          break;
        }
      g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (selection)), >, 0);
      g_assert_true (gtk_widget_is_ancestor (gtk_root_get_focus (GTK_ROOT (run->window)), view));
      g_signal_emit_by_name (search_button, "clicked");
      g_assert_true (gtk_search_bar_get_search_mode (search_bar));
      GtkWidget *search = find_type (GTK_WIDGET (run->window), GTK_TYPE_SEARCH_ENTRY);
      g_assert_true (gtk_widget_is_ancestor (gtk_root_get_focus (GTK_ROOT (run->window)), search));
      g_signal_emit_by_name (search_button, "clicked");
      g_assert_false (gtk_search_bar_get_search_mode (search_bar));
      activate (run, "search");
      g_assert_true (gtk_search_bar_get_search_mode (search_bar));
      g_assert_true (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (search_button)));
      g_assert_true (gtk_widget_is_ancestor (gtk_root_get_focus (GTK_ROOT (run->window)), search));
      g_signal_emit_by_name (search, "stop-search");
      g_assert_false (gtk_search_bar_get_search_mode (search_bar));
      g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (search_button)));
      gtk_widget_grab_focus (view);
      run->original = g_list_model_get_item (G_LIST_MODEL (selection), 0);
      gtk_single_selection_set_selected (selection, GTK_INVALID_LIST_POSITION);
      GtkMenuButton *options
          = find_machine_menu (GTK_WIDGET (run->window), pirwa_machine_get_uuid (run->original));
      g_assert_nonnull (options);
      gtk_menu_button_popup (options);
      g_assert_true (run->original == gtk_single_selection_get_selected_item (selection));
      gtk_menu_button_popdown (options);
      run->step++;
      break;
    case 1:
      if (enabled (run, "start"))
        {
          activate (run, "start");
          break;
        }
      if (!enabled (run, "pause"))
        break;
      activate (run, "pause");
      run->step++;
      break;
    case 2:
      if (!enabled (run, "resume"))
        break;
      activate (run, "resume");
      run->step++;
      break;
    case 3:
      if (!enabled (run, "pause"))
        break;
      /* A stale menu target must never operate on the newly selected VM. */
      g_action_group_activate_action (
          G_ACTION_GROUP (run->window), "machine-force-stop",
          g_variant_new_string ("11111111-1111-4111-8111-111111111111"));
      g_assert_null (
          adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (run->window)));
      activate_machine_menu (run, "win.machine-force-stop");
      run->step++;
      break;
    case 4:
      if (!dialog || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      g_assert_true (ADW_IS_ALERT_DIALOG (dialog));
      g_assert_cmpstr (adw_alert_dialog_get_default_response (ADW_ALERT_DIALOG (dialog)), ==,
                       "cancel");
      adw_dialog_close (dialog);
      run->step++;
      break;
    case 5:
      if (dialog || !enabled (run, "shutdown"))
        break;
      activate (run, "shutdown");
      run->step++;
      break;
    case 6:
      if (!enabled (run, "edit"))
        break;
      activate_machine_menu (run, "win.machine-edit");
      run->step++;
      break;
    case 7:
      if (!dialog || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      {
        GtkWidget *button = find_button (GTK_WIDGET (dialog), "Save Changes");
        g_assert_nonnull (button);
        if (!gtk_widget_get_mapped (button))
          break;
        assert_visible_button (GTK_WIDGET (dialog), button);
        GtkWidget *entry = find_type (GTK_WIDGET (dialog), ADW_TYPE_ENTRY_ROW);
        gtk_editable_set_text (GTK_EDITABLE (entry), "Edited <test> & VM");
        GtkWidget *spin = find_type (GTK_WIDGET (dialog), ADW_TYPE_SPIN_ROW);
        adw_spin_row_set_value (ADW_SPIN_ROW (spin), 3);
        g_signal_emit_by_name (button, "clicked");
        run->step++;
      }
      break;
    case 8:
      if (dialog || !enabled (run, "edit"))
        break;
      g_assert_true (run->original == gtk_single_selection_get_selected_item (selection));
      g_assert_true (has_label (GTK_WIDGET (run->window), "Edited <test> & VM"));
      activate_machine_menu (run, "win.machine-remove");
      run->step++;
      break;
    case 9:
      if (!dialog || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      g_assert_true (ADW_IS_ALERT_DIALOG (dialog));
      g_assert_cmpstr (adw_alert_dialog_get_heading (ADW_ALERT_DIALOG (dialog)), ==,
                       "Remove Virtual Machine?");
      adw_dialog_close (dialog);
      run->step++;
      break;
    case 10:
      if (dialog)
        break;
      activate (run, "new");
      run->step++;
      break;
    case 11:
      if (!dialog || !gtk_widget_get_mapped (GTK_WIDGET (dialog)))
        break;
      if (!gtk_widget_get_mapped (find_button (GTK_WIDGET (dialog), "Create")))
        break;
      assert_visible_button (GTK_WIDGET (dialog), find_button (GTK_WIDGET (dialog), "Create"));
      assert_visible_button (GTK_WIDGET (dialog), find_button (GTK_WIDGET (dialog), "Cancel"));
      save_snapshot (run);
      g_signal_emit_by_name (find_button (GTK_WIDGET (dialog), "Cancel"), "clicked");
      run->step++;
      break;
    case 12:
      if (dialog)
        break;
      /* Close with a selected VM and a details request in flight. */
      if (run->domains)
        {
          gtk_single_selection_set_selected (selection, GTK_INVALID_LIST_POSITION);
          gtk_single_selection_set_selected (selection, 0);
        }
      run->watch = 0;
      finish (run);
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
timeout (gpointer data)
{
  Run *run = data;
  g_error ("UI QA timed out at step %d (mapped=%d, new-enabled=%d, first-paint=%" G_GINT64_FORMAT
           ", ready=%" G_GINT64_FORMAT ", frames=%" G_GINT64_FORMAT
           ", renderer=%s, size=%dx%d, active=%d)",
           run->step, gtk_widget_get_mapped (GTK_WIDGET (run->window)), enabled (run, "new"),
           run->first_paint, run->ready, run->frame_count, run->renderer ? run->renderer : "none",
           gtk_widget_get_width (GTK_WIDGET (run->window)),
           gtk_widget_get_height (GTK_WIDGET (run->window)), gtk_window_is_active (run->window));
  return G_SOURCE_REMOVE;
}

static void
qa_activate (GApplication *application, gpointer data)
{
  (void)application;
  Run *run = data;
  gtk_window_present (run->window);
}

int
main (int argc, char **argv)
{
  Run run = { .start = g_get_monotonic_time (), .pss_kib = -1 };
  char *uri = NULL, *snapshot = NULL;
  gboolean large = FALSE, dark = FALSE;
  run.frame_costs = g_array_new (FALSE, FALSE, sizeof (gint64));
  int width = 1040;
  GOptionEntry entries[]
      = { { "uri", 0, 0, G_OPTION_ARG_STRING, &uri, "Connection to profile", "URI" },
          { "snapshot", 0, 0, G_OPTION_ARG_FILENAME, &snapshot, "Save this window only", "FILE" },
          { "scroll", 0, 0, G_OPTION_ARG_NONE, &run.scrolling,
            "Scroll 40 pixels per frame during sampling", NULL },
          { "exercise", 0, 0, G_OPTION_ARG_NONE, &run.exercise, "Exercise the test-driver UI",
            NULL },
          { "boxes-exercise", 0, 0, G_OPTION_ARG_NONE, &run.boxes_exercise,
            "Exercise the first-run GNOME Boxes guide with isolated fixtures", NULL },
          { "duration", 0, 0, G_OPTION_ARG_INT, &run.duration, "Idle sample seconds", "SECONDS" },
          { "large-text", 0, 0, G_OPTION_ARG_NONE, &large, "Double the desktop font size", NULL },
          { "dark", 0, 0, G_OPTION_ARG_NONE, &dark, "Use dark appearance", NULL },
          { "width", 0, 0, G_OPTION_ARG_INT, &width, "Requested window width", "PIXELS" },
          { NULL } };
  g_autoptr (GOptionContext) options = g_option_context_new (NULL);
  g_autoptr (GError) error = NULL;
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }
  if (!uri)
    uri = g_strdup ("test:///default");
  if (run.boxes_exercise)
    run.exercise = TRUE;
  if (run.duration < 0 || run.duration > 300 || width < 600 || width > 1920
      || (run.scrolling && run.duration == 0)
      || (run.exercise && !g_str_has_prefix (uri, "test:///")))
    {
      g_printerr ("Exercise requires test:///; use duration 0–300 and width 600–1920.\n");
      return 2;
    }
  setlocale (LC_ALL, "C");
  if (run.exercise)
    g_setenv ("GTK_A11Y", "test", TRUE);
  if (run.boxes_exercise)
    {
      run.boxes_socket = -1;
      run.boxes_root = g_dir_make_tmp ("pirwa-boxes-ui-XXXXXX", &error);
      g_assert_no_error (error);
      run.boxes_paths = g_new0 (PirwaBoxesPaths, 1);
      run.boxes_paths->native_uri = g_strdup (uri);
      run.boxes_paths->state_file = g_build_filename (run.boxes_root, "state.ini", NULL);
      run.boxes_paths->flatpak_root = g_strdup (run.boxes_root);
      run.boxes_paths->runtime_root = g_build_filename (run.boxes_root, "runtime", NULL);
      g_autofree char *directory
          = g_build_filename (run.boxes_root, "config", "libvirt", "qemu", NULL);
      g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
      g_autofree char *file = g_build_filename (directory, "boxes.xml", NULL);
      g_assert_true (g_file_set_contents (
          file,
          "<domain><name>Flatpak "
          "Example</name><uuid>11111111-1111-4111-8111-111111111111</uuid></domain>",
          -1, &error));
      g_assert_no_error (error);
      g_autofree char *bin = g_build_filename (run.boxes_root, "bin", NULL);
      g_assert_cmpint (g_mkdir (bin, 0700), ==, 0);
      g_autofree char *program = g_build_filename (bin, "flatpak", NULL);
      run.boxes_paths->flatpak_program = g_strdup (program);
      g_assert_true (g_file_set_contents (program, "#!/bin/sh\nexit 0\n", -1, &error));
      g_assert_no_error (error);
      g_assert_cmpint (g_chmod (program, 0700), ==, 0);
      g_autofree char *path = g_strconcat (bin, ":", g_getenv ("PATH"), NULL);
      g_setenv ("PATH", path, TRUE);
    }
  if (!gtk_init_check ())
    {
      g_printerr ("A desktop display is required for explicit UI QA.\n");
      return 77;
    }
  adw_init ();
  if (large)
    {
      g_autofree char *font = NULL;
      g_object_get (gtk_settings_get_default (), "gtk-font-name", &font, NULL);
      PangoFontDescription *description = pango_font_description_from_string (font);
      pango_font_description_set_size (description,
                                       pango_font_description_get_size (description) * 2);
      g_autofree char *scaled = pango_font_description_to_string (description);
      g_object_set (gtk_settings_get_default (), "gtk-font-name", scaled, NULL);
      pango_font_description_free (description);
    }
  if (dark)
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        ADW_COLOR_SCHEME_FORCE_DARK);
  if (run.exercise)
    g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  /* Unexpected GTK/GObject warnings in these paths are test failures. */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  g_autoptr (AdwApplication) application
      = adw_application_new ("io.github.pirwa.Pirwa.QA", G_APPLICATION_NON_UNIQUE);
  g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
  g_assert_no_error (error);
  run.application = GTK_APPLICATION (application);
  run.snapshot = snapshot;
  run.window = run.boxes_paths ? pirwa_window_new_with_boxes (run.application, uri, run.boxes_paths)
                               : pirwa_window_new (run.application, uri);
  run.requested_width = width;
  gtk_window_set_default_size (run.window, width, 680);
  g_signal_connect (run.window, "map", G_CALLBACK (mapped), &run);
  g_signal_connect (application, "activate", G_CALLBACK (qa_activate), &run);
  run.watch = g_timeout_add (10, watch, &run);
  run.timeout = g_timeout_add_seconds (run.duration + 20, timeout, &run);
  int result = g_application_run (G_APPLICATION (application), 0, NULL);
  if (run.watch)
    g_source_remove (run.watch);
  g_source_remove (run.timeout);
  if (run.clock)
    {
      g_signal_handler_disconnect (run.clock, run.paint_handler);
      g_signal_handler_disconnect (run.clock, run.before_handler);
      g_object_unref (run.clock);
    }
  g_clear_object (&run.original);
  g_assert_true (run.finished);
  double cpu = run.idle_wall_us > 0 ? run.idle_cpu_ns / (run.idle_wall_us * 10.0) : 0;
  g_print (
      "{\"main_monotonic_us\":%" G_GINT64_FORMAT ",\"first_paint_monotonic_us\":%" G_GINT64_FORMAT
      ",\"ready_monotonic_us\":%" G_GINT64_FORMAT ",\"main_to_first_paint_ms\":%.3f,"
      "\"sample_wall_ms\":%.3f,\"sample_cpu_percent\":%.4f,\"sample_frames\":%" G_GINT64_FORMAT
      ",\"pss_kib\":%" G_GINT64_FORMAT ",\"exercise_steps\":%d,\"domains\":%u,\"renderer\":\"%s\","
      "\"text_scale\":%d,\"scroll\":%s,\"focus_kind\":\"%s\",\"initial_focus_kind\":\"%s\","
      "\"active\":%s,\"refresh_millihz\":%d,\"surface_scale\":%d,\"frame_work_us\":[",
      run.start, run.first_paint, run.ready, (run.first_paint - run.start) / 1000.0,
      run.idle_wall_us / 1000.0, cpu, run.idle_frames, run.pss_kib, run.step, run.domains,
      run.renderer, large ? 2 : 1, run.scrolling ? "true" : "false",
      run.focus_kind ? run.focus_kind : "not-sampled",
      run.initial_focus_kind ? run.initial_focus_kind : "not-sampled",
      run.window_active ? "true" : "false", run.refresh_millihz, run.surface_scale);
  for (guint i = 0; i < run.frame_costs->len; i++)
    g_print ("%s%" G_GINT64_FORMAT, i ? "," : "", g_array_index (run.frame_costs, gint64, i));
  g_print ("]}\n");
  g_array_unref (run.frame_costs);
  g_free (run.renderer);
  g_free (run.focus_kind);
  g_free (run.initial_focus_kind);
  g_free (uri);
  g_free (snapshot);
  if (run.boxes_root)
    {
      if (run.boxes_socket >= 0)
        close (run.boxes_socket);
      const char *files[] = { "state.ini", "bin/flatpak", "config/libvirt/qemu/boxes.xml",
                              "runtime/libvirt/libvirt-sock" };
      const char *dirs[] = { "bin",    "config/libvirt/qemu", "config/libvirt",
                             "config", "runtime/libvirt",     "runtime" };
      for (guint i = 0; i < G_N_ELEMENTS (files); i++)
        {
          g_autofree char *file = g_build_filename (run.boxes_root, files[i], NULL);
          g_unlink (file);
        }
      for (guint i = 0; i < G_N_ELEMENTS (dirs); i++)
        {
          g_autofree char *dir = g_build_filename (run.boxes_root, dirs[i], NULL);
          g_rmdir (dir);
        }
      g_rmdir (run.boxes_root);
    }
  pirwa_boxes_paths_free (run.boxes_paths);
  g_free (run.boxes_root);
  return result;
}
