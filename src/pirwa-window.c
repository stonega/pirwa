/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-window.h"
#include "pirwa-inventory.h"

struct _PirwaWindow
{
  AdwApplicationWindow parent_instance;
  char *uri;
  GCancellable *cancellable;
  GListStore *machines;
  GtkStack *stack;
  GtkWidget *refresh;
  AdwStatusPage *error_page;
};

G_DEFINE_FINAL_TYPE (PirwaWindow, pirwa_window, ADW_TYPE_APPLICATION_WINDOW)

static void
factory_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void)factory;
  (void)user_data;
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
  g_object_set_data (G_OBJECT (item), "name-label", name);
  g_object_set_data (G_OBJECT (item), "state-label", state);
  gtk_list_item_set_child (item, row);
}

static void
factory_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void)factory;
  (void)user_data;
  PirwaMachine *machine = PIRWA_MACHINE (gtk_list_item_get_item (item));
  GtkWidget *row = gtk_list_item_get_child (item);
  GtkLabel *name = g_object_get_data (G_OBJECT (item), "name-label");
  GtkLabel *state = g_object_get_data (G_OBJECT (item), "state-label");
  gtk_label_set_text (name, pirwa_machine_get_name (machine));
  gtk_label_set_text (state, pirwa_machine_get_state (machine));
  gtk_widget_set_tooltip_text (row, pirwa_machine_get_uuid (machine));
}

static void
inventory_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  g_autoptr (PirwaWindow) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) machines = pirwa_inventory_load_finish (result, &error);

  /* A closed window must never be touched by a late worker completion. */
  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;

  gtk_widget_set_sensitive (self->refresh, TRUE);
  if (machines == NULL)
    {
      g_autofree char *description = g_markup_escape_text (error->message, -1);
      adw_status_page_set_description (self->error_page, description);
      gtk_stack_set_visible_child_name (self->stack, "error");
      return;
    }

  g_list_store_splice (self->machines, 0, g_list_model_get_n_items (G_LIST_MODEL (self->machines)),
                       machines->pdata, machines->len);
  gtk_stack_set_visible_child_name (self->stack, machines->len ? "machines" : "empty");
}

static void
refresh_inventory (PirwaWindow *self)
{
  /* Only one discovery request can be in flight for this window. */
  gtk_widget_set_sensitive (self->refresh, FALSE);
  gtk_stack_set_visible_child_name (self->stack, "loading");
  pirwa_inventory_load_async (self->uri, self->cancellable, inventory_loaded, g_object_ref (self));
}

static gboolean
window_close_requested (GtkWindow *window, gpointer user_data)
{
  (void)user_data;
  g_cancellable_cancel (PIRWA_WINDOW (window)->cancellable);
  return FALSE;
}

static void
pirwa_window_dispose (GObject *object)
{
  PirwaWindow *self = PIRWA_WINDOW (object);
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->machines);
  G_OBJECT_CLASS (pirwa_window_parent_class)->dispose (object);
}

static void
pirwa_window_finalize (GObject *object)
{
  g_free (PIRWA_WINDOW (object)->uri);
  G_OBJECT_CLASS (pirwa_window_parent_class)->finalize (object);
}

static void
pirwa_window_class_init (PirwaWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = pirwa_window_dispose;
  object_class->finalize = pirwa_window_finalize;
}

static void
pirwa_window_init (PirwaWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *title = adw_window_title_new ("Pirwa", "Virtual Machines");
  GtkWidget *empty = adw_status_page_new ();
  GtkWidget *loading = adw_status_page_new ();
  GtkWidget *spinner = gtk_spinner_new ();
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();

  self->cancellable = g_cancellable_new ();
  self->machines = g_list_store_new (PIRWA_TYPE_MACHINE);
  self->stack = GTK_STACK (gtk_stack_new ());
  self->refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  self->error_page = ADW_STATUS_PAGE (adw_status_page_new ());

  gtk_window_set_title (GTK_WINDOW (self), "Pirwa");
  gtk_window_set_default_size (GTK_WINDOW (self), 880, 600);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), title);
  gtk_widget_set_tooltip_text (self->refresh, "Refresh virtual machines");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->refresh), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Refresh virtual machines", -1);
  g_signal_connect_swapped (self->refresh, "clicked", G_CALLBACK (refresh_inventory), self);
  g_signal_connect (self, "close-request", G_CALLBACK (window_close_requested), NULL);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->refresh);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  gtk_spinner_start (GTK_SPINNER (spinner));
  adw_status_page_set_title (ADW_STATUS_PAGE (loading), "Loading Virtual Machines");
  adw_status_page_set_child (ADW_STATUS_PAGE (loading), spinner);
  gtk_stack_add_named (self->stack, loading, "loading");

  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "computer-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Virtual Machines");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty),
                                   "Virtual machines on this connection will appear here.");
  gtk_stack_add_named (self->stack, empty, "empty");

  adw_status_page_set_icon_name (self->error_page, "network-error-symbolic");
  adw_status_page_set_title (self->error_page, "Connection Unavailable");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->error_page), "error");

  g_signal_connect (factory, "setup", G_CALLBACK (factory_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (factory_bind), NULL);
  GtkSelectionModel *selection
      = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->machines))));
  GtkWidget *list = gtk_list_view_new (selection, factory);
  gtk_list_view_set_show_separators (GTK_LIST_VIEW (list), TRUE);
  gtk_widget_add_css_class (list, "navigation-sidebar");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), list);
  gtk_stack_add_named (self->stack, scrolled, "machines");

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toolbar);
}

GtkWindow *
pirwa_window_new (GtkApplication *application, const char *uri)
{
  PirwaWindow *self = g_object_new (PIRWA_TYPE_WINDOW, "application", application, NULL);
  self->uri = g_strdup (uri);
  refresh_inventory (self);
  return GTK_WINDOW (self);
}
