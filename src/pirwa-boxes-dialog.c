/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-boxes-dialog.h"
#include "config.h"
#include <glib/gi18n.h>

typedef struct
{
  AdwDialog *dialog;
  PirwaBoxesScan *scan;
  GtkLabel *explanation;
  GtkLabel *error;
  GtkSingleSelection *selection;
  GtkWidget *apply;
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
  return guide->scan->libraries->len ? g_ptr_array_index (guide->scan->libraries, 0) : NULL;
}

static void
respond (Guide *guide, PirwaBoxesResponse response)
{
  if (guide->responded)
    return;
  if (response == PIRWA_BOXES_SKIP || response == PIRWA_BOXES_RESCAN)
    guide->responded = TRUE;
  g_autoptr (GtkWindow) parent = g_weak_ref_get (&guide->parent);
  PirwaBoxesLibrary *library = selected_library (guide);
  PirwaMachine *machine = gtk_single_selection_get_selected_item (guide->selection);
  if (parent)
    guide->response (parent, guide->dialog, response,
                     response == PIRWA_BOXES_IMPORT && library ? library->uri : NULL,
                     response == PIRWA_BOXES_IMPORT && machine ? pirwa_machine_get_uuid (machine)
                                                               : NULL);
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
  if (response == PIRWA_BOXES_SKIP && !adw_dialog_get_can_close (guide->dialog))
    {
      respond (guide, PIRWA_BOXES_CANCEL);
      return;
    }
  respond (guide, response);
  if (response != PIRWA_BOXES_IMPORT)
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
      gtk_widget_set_visible (GTK_WIDGET (guide->error), TRUE);
    }
}

static void
open_boxes (GtkButton *button, Guide *guide)
{
  (void)button;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) process = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "flatpak",
                                                      "run", "org.gnome.Boxes", NULL);
  gtk_widget_set_visible (GTK_WIDGET (guide->error), TRUE);
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
populate_guide (Guide *guide)
{
  PirwaBoxesLibrary *library = selected_library (guide);
  g_autoptr (GListStore) store = g_list_store_new (PIRWA_TYPE_MACHINE);
  if (library)
    for (guint i = 0; i < library->machines->len; i++)
      g_list_store_append (store, g_ptr_array_index (library->machines, i));
  gtk_single_selection_set_model (guide->selection, G_LIST_MODEL (store));
  gtk_widget_set_sensitive (guide->apply, library && library->available && library->machines->len);
  gtk_widget_set_visible (guide->open, guide->scan->installed && (!library || !library->available));
  gtk_widget_set_visible (guide->rescan, guide->scan->installed);
  gtk_label_set_text (
      guide->explanation,
      !guide->scan->installed
          ? _ ("GNOME Boxes Flatpak is not installed. Install org.gnome.Boxes to import its "
               "machines. Native Boxes machines already appear in Session.")
      : !library
          ? _ ("No machines were found in Boxes Flatpak. Create a machine in Boxes, then choose "
               "Refresh Library. Native Boxes machines already appear in Session.")
      : !library->available
          ? _ ("Open Boxes Flatpak so Pirwa can read its library. Shut down the machine you want "
               "to import, leave Boxes open, then choose Refresh Library.")
          : _ ("Choose a machine to copy into your Session library. Shut it down in Boxes first, "
               "and leave Boxes open without starting the guest until import finishes."));
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
  adw_dialog_set_title (guide->dialog, _ ("Import from Boxes Flatpak"));
  adw_dialog_set_content_width (guide->dialog, 620);
  adw_dialog_set_content_height (guide->dialog, 560);
  g_object_set_data_full (G_OBJECT (guide->dialog), "boxes-guide", guide, guide_free);
  g_signal_connect (guide->dialog, "closed", G_CALLBACK (closed), guide);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  GtkWidget *cancel = gtk_button_new_with_label (_ ("Not Now"));
  g_object_set_data (G_OBJECT (guide->dialog), "cancel", cancel);
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
  guide->error = GTK_LABEL (gtk_label_new (scan->warning));
  gtk_widget_set_visible (GTK_WIDGET (guide->error), scan->warning != NULL);
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
  g_object_set_data (G_OBJECT (guide->dialog), "rescan", guide->rescan);
  GtkWidget *progress = gtk_progress_bar_new ();
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (progress), TRUE);
  gtk_widget_set_visible (progress, FALSE);
  g_object_set_data (G_OBJECT (guide->dialog), "progress", progress);
  guide->selection = gtk_single_selection_new (NULL);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (row_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (row_bind), NULL);
  GtkWidget *list
      = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (guide->selection)), factory);
  gtk_widget_add_css_class (list, "boxed-list");
  g_object_set_data (G_OBJECT (guide->dialog), "machines", list);
  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  PirwaBoxesLibrary *library = selected_library (guide);
  gtk_scrolled_window_set_min_content_height (
      GTK_SCROLLED_WINDOW (list_scroll),
      library ? MIN (200, MAX (48, library->machines->len * 48)) : 48);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (list_scroll), 200);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (list_scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), list);
  gtk_widget_set_visible (list_scroll, scan->libraries->len > 0);
  gtk_box_append (GTK_BOX (content), list_scroll);
  GtkWidget *note = gtk_label_new (_ (
      "Pirwa copies the disk and UEFI firmware state into its own storage. Allow enough free "
      "space for a separate disk copy; large disks can take a while. You can cancel the copy.\n\n"
      "The imported machine appears stopped in Session and works without Boxes. Select Start "
      "to check it. The original machine and its files stay in Boxes; remove them there only "
      "after checking the imported copy. Installation media is ejected from the copy.\n\n"
      "Import currently supports one standalone qcow2 disk. Machines with snapshots, saved "
      "memory, backing chains, TPM, shared folders or passthrough devices are not supported."));
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_xalign (GTK_LABEL (note), 0);
  gtk_box_append (GTK_BOX (content), note);
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), content);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scrolled);
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (footer, 18);
  gtk_widget_set_margin_end (footer, 18);
  gtk_widget_set_margin_top (footer, 12);
  gtk_widget_set_margin_bottom (footer, 12);
  gtk_box_append (GTK_BOX (footer), progress);
  guide->apply = gtk_button_new_with_label (_ ("Import Machine"));
  g_object_set_data (G_OBJECT (guide->dialog), "apply", guide->apply);
  g_object_set_data (G_OBJECT (guide->dialog), "error", guide->error);
  gtk_widget_add_css_class (guide->apply, "suggested-action");
  g_object_set_data (G_OBJECT (guide->apply), "response", GINT_TO_POINTER (PIRWA_BOXES_IMPORT));
  g_signal_connect (guide->apply, "clicked", G_CALLBACK (button_clicked), guide);
  gtk_box_append (GTK_BOX (footer), guide->apply);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), footer);
  populate_guide (guide);
  adw_dialog_set_child (guide->dialog, toolbar);
  adw_dialog_present (guide->dialog, GTK_WIDGET (parent));
}
