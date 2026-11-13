/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "pirwa-inventory.h"
#include "pirwa-window.h"

static void
activate (GtkApplication *application, gpointer user_data)
{
  (void)user_data;
  GtkWindow *window = gtk_application_get_active_window (application);
  if (window == NULL)
    {
      const char *uri = g_getenv ("PIRWA_LIBVIRT_URI");
      window = pirwa_window_new (application, uri && *uri ? uri : PIRWA_DEFAULT_URI);
    }
  gtk_window_present (window);
}

static int
handle_local_options (GApplication *application, GVariantDict *options, gpointer user_data)
{
  (void)application;
  (void)user_data;
  if (g_variant_dict_contains (options, "version"))
    {
      g_print ("Pirwa %s\n", PIRWA_VERSION);
      return 0;
    }
  return -1;
}

int
main (int argc, char **argv)
{
  g_set_application_name ("Pirwa");
  g_set_prgname ("pirwa");
  g_autoptr (AdwApplication) application
      = adw_application_new (PIRWA_APPLICATION_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option (G_APPLICATION (application), "version", 0, G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE, "Show the application version", NULL);
  g_signal_connect (application, "handle-local-options", G_CALLBACK (handle_local_options), NULL);
  g_signal_connect (application, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (application), argc, argv);
}
