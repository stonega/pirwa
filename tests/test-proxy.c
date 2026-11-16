/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-proxy.h"

static void
set_endpoint (const char *protocol, const char *host, int port)
{
  g_autofree char *id = g_strdup_printf ("org.gnome.system.proxy.%s", protocol);
  g_autoptr (GSettings) settings = g_settings_new (id);
  g_settings_set_string (settings, "host", host);
  g_settings_set_int (settings, "port", port);
}

static void
test_desktop (void)
{
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.system.proxy");
  g_autoptr (GError) error = NULL;
  g_assert_null (pirwa_proxy_read (&error));
  g_assert_no_error (error);
  g_settings_set_string (settings, "mode", "auto");
  g_assert_null (pirwa_proxy_read (&error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_settings_set_string (settings, "mode", "manual");
  g_assert_null (pirwa_proxy_read (&error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  set_endpoint ("socks", "127.0.0.1", 7890);
  g_autoptr (PirwaProxy) socks = pirwa_proxy_read (&error);
  g_assert_no_error (error);
  g_assert_cmpstr (socks->default_uri, ==, "socks5://127.0.0.1:7890");
  g_assert_cmpstr (socks->ignore_hosts[1], ==, "127.0.0.0/8");
  g_autofree char *serialized = pirwa_proxy_serialize (socks);
  g_autoptr (PirwaProxy) roundtrip = pirwa_proxy_parse (serialized, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (roundtrip->default_uri, ==, socks->default_uri);
  g_assert_cmpstr (roundtrip->dns, ==, "1.1.1.1");
  set_endpoint ("socks", "", 0);
  set_endpoint ("http", "127.0.0.1", 3128);
  set_endpoint ("https", "::1", 3129);
  g_autoptr (GSettings) http = g_settings_new ("org.gnome.system.proxy.http");
  g_settings_set_boolean (http, "use-authentication", TRUE);
  g_settings_set_string (http, "authentication-user", "test @user");
  g_settings_set_string (http, "authentication-password", "pass:/?#word");
  g_autoptr (PirwaProxy) authenticated = pirwa_proxy_read (&error);
  g_assert_no_error (error);
  g_assert_cmpstr (authenticated->https_uri, ==, "http://[::1]:3129");
  g_autoptr (GUri) uri = g_uri_parse (authenticated->default_uri, G_URI_FLAGS_HAS_PASSWORD, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (g_uri_get_user (uri), ==, "test @user");
  g_assert_cmpstr (g_uri_get_password (uri), ==, "pass:/?#word");
  set_endpoint ("https", "127.0.0.1", 3128);
  g_autoptr (PirwaProxy) shared = pirwa_proxy_read (&error);
  g_assert_no_error (error);
  g_assert_cmpstr (shared->https_uri, ==, shared->default_uri);
  set_endpoint ("socks", "bad/host", 7890);
  g_assert_null (pirwa_proxy_read (&error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  set_endpoint ("socks", "localhost", 65536);
  g_assert_null (pirwa_proxy_read (&error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_invalid (void)
{
  const char *invalid[]
      = { "('direct://', '', '1.1.1.1', @as [])", "('socks5://localhost:0', '', '1.1.1.1', @as [])",
          "('socks5://localhost:7890', '', 'bad-dns', @as [])", "not a configuration",
          "('', '', '', @as [])" };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autoptr (GError) error = NULL;
      g_assert_null (pirwa_proxy_parse (invalid[i], &error));
      g_assert_nonnull (error);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/proxy/desktop", test_desktop);
  g_test_add_func ("/proxy/invalid", test_invalid);
  return g_test_run ();
}
