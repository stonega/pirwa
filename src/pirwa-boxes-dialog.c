/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-boxes-dialog.h"
#include "config.h"
#include <glib/gi18n.h>

typedef struct
{
  AdwDialog *dialog;
  PirwaBoxesScan *scan;
  GtkDropDown *source;
  GtkLabel *explanation;
  GtkLabel *error;
  GtkSingleSelection *selection;
  GtkWidget *use;
  GtkWidget *open;
  GtkWidget *rescan;
  GWeakRef parent;
  PirwaBoxesResponseFunc response;
  gboolean responded;
  GCancellable *launch_cancel;
} Guide;

static void
guide_free (gpointer data)
{
  Guide *guide = data;
  g_weak_ref_clear (&guide->parent);
  g_object_unref (guide->selection);
  pirwa_boxes_scan_free (guide->scan);
  g_clear_object (&guide->launch_cancel);
  g_free (guide);
}

static PirwaBoxesLibrary *
selected_library (Guide *guide)
{
  guint index = gtk_drop_down_get_selected (guide->source);
  return index < guide->scan->libraries->len ? g_ptr_array_index (guide->scan->libraries, index)
                                             : NULL;
}

static void
respond (Guide *guide, PirwaBoxesResponse response)
{
  if (guide->responded)
    return;
  guide->responded = TRUE;
  g_autoptr (GtkWindow) parent = g_weak_ref_get (&guide->parent);
  PirwaBoxesLibrary *library = selected_library (guide);
  PirwaMachine *machine = gtk_single_selection_get_selected_item (guide->selection);
  if (parent)
    guide->response (parent, response, response == PIRWA_BOXES_USE && library ? library->uri : NULL,
                     response == PIRWA_BOXES_USE && machine ? pirwa_machine_get_uuid (machine)
                                                            : NULL,
                     response == PIRWA_BOXES_USE && library ? library->flatpak
                                                            : guide->scan->preferred_uri != NULL);
}

static void
closed (AdwDialog *dialog, Guide *guide)
{
  (void)dialog;
  g_cancellable_cancel (guide->launch_cancel);
  respond (guide, PIRWA_BOXES_SKIP);
}

static void
button_clicked (GtkButton *button, Guide *guide)
{
  PirwaBoxesResponse response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "response"));
  respond (guide, response);
  adw_dialog_close (guide->dialog);
}

static void
boxes_started (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (AdwDialog) dialog = data;
  g_autoptr (GError) error = NULL;
  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error)
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      && gtk_widget_get_mapped (GTK_WIDGET (dialog)))
    {
      Guide *guide = g_object_get_data (G_OBJECT (dialog), "boxes-guide");
      g_autofree char *message = g_strdup_printf (
          _ ("GNOME Boxes could not start. Check that its Flatpak is installed.\n%s"),
          error->message);
      gtk_label_set_text (guide->error, message);
    }
}

static void
open_boxes (GtkButton *button, Guide *guide)
{
  (void)button;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) process = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "flatpak",
                                                      "run", "org.gnome.Boxes", NULL);
  if (!process)
    gtk_label_set_text (guide->error, error->message);
  else
    {
      gtk_label_set_text (guide->error, _ ("After GNOME Boxes opens, choose Refresh Library."));
      g_subprocess_wait_check_async (process, guide->launch_cancel, boxes_started,
                                     g_object_ref (guide->dialog));
    }
}

static void
source_changed (GObject *object, GParamSpec *spec, Guide *guide)
{
  (void)object;
  (void)spec;
  PirwaBoxesLibrary *library = selected_library (guide);
  g_autoptr (GListStore) store = g_list_store_new (PIRWA_TYPE_MACHINE);
  if (library)
    for (guint i = 0; i < library->machines->len; i++)
      g_list_store_append (store, g_ptr_array_index (library->machines, i));
  gtk_single_selection_set_model (guide->selection, G_LIST_MODEL (store));
  gtk_widget_set_sensitive (guide->use, library && library->available);
  gtk_widget_set_visible (guide->open, library && library->flatpak && !library->available);
  gtk_widget_set_visible (guide->rescan, library && library->flatpak);
  gtk_label_set_text (
      guide->explanation,
      !library ? _ ("No local GNOME Boxes machines were found. Install GNOME Boxes or create a "
                    "machine there, then try Import from GNOME Boxes again.")
      : library->flatpak
          ? _ ("These machines belong to the GNOME Boxes Flatpak library. Keep Boxes running so "
               "Pirwa can connect. Open Boxes if needed, then refresh this library.")
          : _ ("GNOME Boxes and Pirwa share the Session library. Use these existing machines "
               "directly, without copying disks or changing their configuration."));
}

static void
row_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_top (label, 10);
  gtk_widget_set_margin_bottom (label, 10);
  gtk_widget_set_margin_start (label, 12);
  gtk_widget_set_margin_end (label, 12);
  gtk_list_item_set_child (item, label);
}

static void
row_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  PirwaMachine *machine = gtk_list_item_get_item (item);
  GtkWidget *label = gtk_list_item_get_child (item);
  gtk_label_set_text (GTK_LABEL (label), pirwa_machine_get_name (machine));
  gtk_widget_set_tooltip_text (label, pirwa_machine_get_name (machine));
}

void
pirwa_boxes_dialog_present (GtkWindow *parent, PirwaBoxesScan *scan,
                            PirwaBoxesResponseFunc response)
{
  Guide *guide = g_new0 (Guide, 1);
  guide->scan = scan;
  guide->response = response;
  guide->launch_cancel = g_cancellable_new ();
  g_weak_ref_init (&guide->parent, parent);
  guide->dialog = adw_dialog_new ();
  adw_dialog_set_title (guide->dialog, _ ("Import from GNOME Boxes"));
  adw_dialog_set_content_width (guide->dialog, 620);
  adw_dialog_set_content_height (guide->dialog, 560);
  g_object_set_data_full (G_OBJECT (guide->dialog), "boxes-guide", guide, guide_free);
  g_signal_connect (guide->dialog, "closed", G_CALLBACK (closed), guide);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Not Now"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (button_clicked), guide);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_top (content, 18);
  gtk_widget_set_margin_bottom (content, 18);
  gtk_widget_set_margin_start (content, 18);
  gtk_widget_set_margin_end (content, 18);
  guide->explanation = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (guide->explanation, TRUE);
  gtk_label_set_xalign (guide->explanation, 0);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (guide->explanation));
  GtkStringList *sources = gtk_string_list_new (NULL);
  for (guint i = 0; i < scan->libraries->len; i++)
    {
      PirwaBoxesLibrary *library = g_ptr_array_index (scan->libraries, i);
      g_autofree char *label = g_strdup_printf (
          ngettext ("%s · %u machine", "%s · %u machines", library->machines->len),
          library->flatpak ? _ ("Boxes Flatpak") : _ ("Session"), library->machines->len);
      gtk_string_list_append (sources, label);
    }
  guide->source = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (sources), NULL));
  gtk_accessible_update_property (GTK_ACCESSIBLE (guide->source), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("GNOME Boxes library"), -1);
  gtk_widget_set_visible (GTK_WIDGET (guide->source), scan->libraries->len > 0);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (guide->source));
  guide->selection = gtk_single_selection_new (NULL);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (row_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (row_bind), NULL);
  GtkWidget *list
      = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (guide->selection)), factory);
  gtk_widget_add_css_class (list, "boxed-list");
  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (list_scroll), 120);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (list_scroll), 200);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (list_scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), list);
  gtk_widget_set_visible (list_scroll, scan->libraries->len > 0);
  gtk_box_append (GTK_BOX (content), list_scroll);
  GtkWidget *note = gtk_label_new (
      _ ("Both apps control the same machines. Disks stay in their existing location. Import does "
         "not grant Pirwa permission to delete Boxes disks."));
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_xalign (GTK_LABEL (note), 0);
  gtk_box_append (GTK_BOX (content), note);
  guide->error = GTK_LABEL (gtk_label_new (scan->warning));
  gtk_label_set_wrap (guide->error, TRUE);
  gtk_label_set_wrap_mode (guide->error, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (guide->error, 0);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (guide->error));
  guide->open = gtk_button_new_with_label (_ ("Open GNOME Boxes"));
  g_signal_connect (guide->open, "clicked", G_CALLBACK (open_boxes), guide);
  gtk_box_append (GTK_BOX (content), guide->open);
  guide->rescan = gtk_button_new_with_label (_ ("Refresh Library"));
  g_object_set_data (G_OBJECT (guide->rescan), "response", GINT_TO_POINTER (PIRWA_BOXES_RESCAN));
  g_signal_connect (guide->rescan, "clicked", G_CALLBACK (button_clicked), guide);
  gtk_box_append (GTK_BOX (content), guide->rescan);
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), content);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scrolled);
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (footer, 18);
  gtk_widget_set_margin_end (footer, 18);
  gtk_widget_set_margin_top (footer, 12);
  gtk_widget_set_margin_bottom (footer, 12);
  guide->use = gtk_button_new_with_label (_ ("Use Machines"));
  gtk_widget_add_css_class (guide->use, "suggested-action");
  g_object_set_data (G_OBJECT (guide->use), "response", GINT_TO_POINTER (PIRWA_BOXES_USE));
  g_signal_connect (guide->use, "clicked", G_CALLBACK (button_clicked), guide);
  gtk_box_append (GTK_BOX (footer), guide->use);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), footer);
  g_signal_connect (guide->source, "notify::selected", G_CALLBACK (source_changed), guide);
  source_changed (NULL, NULL, guide);
  adw_dialog_set_child (guide->dialog, toolbar);
  adw_dialog_present (guide->dialog, GTK_WIDGET (parent));
}
