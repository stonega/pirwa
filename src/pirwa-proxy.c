/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-proxy.h"

static GSettings *
desktop_settings (const char *id)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema
      = source ? g_settings_schema_source_lookup (source, id, TRUE) : NULL;
  return schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
}

static char *
proxy_uri (const char *protocol, const char *scheme, GError **error)
{
  g_autofree char *id = g_strdup_printf ("org.gnome.system.proxy.%s", protocol);
  g_autoptr (GSettings) settings = desktop_settings (id);
  if (!settings)
    return NULL;
  g_autofree char *host = g_settings_get_string (settings, "host");
  int port = g_settings_get_int (settings, "port");
  if (!*host)
    return NULL;
  if (port < 1 || port > 65535 || strpbrk (host, "/@?# \t\r\n"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "The desktop proxy has an invalid host or port");
      return NULL;
    }
  g_autofree char *user = NULL;
  g_autofree char *password = NULL;
  if (g_str_equal (protocol, "http") && g_settings_get_boolean (settings, "use-authentication"))
    {
      user = g_settings_get_string (settings, "authentication-user");
      password = g_settings_get_string (settings, "authentication-password");
    }
  return g_uri_join_with_user (G_URI_FLAGS_NONE, scheme, user, password, NULL, host, port, "", NULL,
                               NULL);
}

PirwaProxy *
pirwa_proxy_read (GError **error)
{
  g_autoptr (GSettings) settings = desktop_settings ("org.gnome.system.proxy");
  if (!settings)
    return NULL;
  g_autofree char *mode = g_settings_get_string (settings, "mode");
  if (g_str_equal (mode, "none"))
    return NULL;
  if (!g_str_equal (mode, "manual"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Automatic VM proxy routing requires a manual desktop proxy; "
                           "PAC proxy configuration is not supported");
      return NULL;
    }
  g_autoptr (PirwaProxy) proxy = g_new0 (PirwaProxy, 1);
  g_autoptr (GError) local_error = NULL;
  proxy->default_uri = proxy_uri ("socks", "socks5", &local_error);
  if (local_error)
    goto fail;
  if (!proxy->default_uri)
    {
      proxy->default_uri = proxy_uri ("http", "http", &local_error);
      if (local_error)
        goto fail;
      proxy->https_uri = proxy_uri ("https", "http", &local_error);
      if (local_error)
        goto fail;
      if (!proxy->default_uri && proxy->https_uri)
        proxy->default_uri = g_strdup (proxy->https_uri);
      if (proxy->default_uri && proxy->https_uri)
        {
          g_autoptr (GUri) http = g_uri_parse (proxy->default_uri, G_URI_FLAGS_HAS_PASSWORD, NULL);
          g_autoptr (GUri) https = g_uri_parse (proxy->https_uri, G_URI_FLAGS_NONE, NULL);
          if (http && https && g_uri_get_user (http)
              && g_strcmp0 (g_uri_get_host (http), g_uri_get_host (https)) == 0
              && g_uri_get_port (http) == g_uri_get_port (https))
            {
              g_free (proxy->https_uri);
              proxy->https_uri = g_strdup (proxy->default_uri);
            }
        }
    }
  if (!proxy->default_uri)
    {
      g_set_error_literal (&local_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Set a SOCKS or HTTP desktop proxy before starting this machine");
      goto fail;
    }
  proxy->ignore_hosts = g_settings_get_strv (settings, "ignore-hosts");
  /* DNS goes through the same tunnel, never through the host's UDP resolver. */
  proxy->dns = g_strdup ("1.1.1.1");
  return g_steal_pointer (&proxy);
fail:
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;
}

char *
pirwa_proxy_serialize (const PirwaProxy *proxy)
{
  g_autoptr (GVariant) value = g_variant_ref_sink (
      g_variant_new ("(sss^as)", proxy->default_uri, proxy->https_uri ? proxy->https_uri : "",
                     proxy->dns, proxy->ignore_hosts));
  return g_variant_print (value, FALSE);
}

PirwaProxy *
pirwa_proxy_parse (const char *data, GError **error)
{
  /* GVariant parser diagnostics can include their input. Do not return a
   * diagnostic containing the serialized proxy authentication details. */
  g_autoptr (GVariant) value = g_variant_parse (G_VARIANT_TYPE ("(sssas)"), data, NULL, NULL, NULL);
  if (!value)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Invalid VM proxy configuration");
      return NULL;
    }
  g_autoptr (PirwaProxy) proxy = g_new0 (PirwaProxy, 1);
  g_autoptr (GInetAddress) dns = NULL;
  g_variant_get (value, "(sss^as)", &proxy->default_uri, &proxy->https_uri, &proxy->dns,
                 &proxy->ignore_hosts);
  const char *uris[] = { proxy->default_uri, proxy->https_uri };
  for (guint i = 0; i < G_N_ELEMENTS (uris); i++)
    {
      if (i && !*uris[i])
        continue;
      g_autoptr (GUri) uri = g_uri_parse (uris[i], G_URI_FLAGS_HAS_PASSWORD, NULL);
      if (!uri || !g_uri_get_host (uri) || g_uri_get_port (uri) < 1
          || (!g_str_equal (g_uri_get_scheme (uri), "socks5")
              && !g_str_equal (g_uri_get_scheme (uri), "http")))
        goto invalid;
    }
  dns = g_inet_address_new_from_string (proxy->dns);
  if (!dns || g_inet_address_get_family (dns) != G_SOCKET_FAMILY_IPV4)
    goto invalid;
  return g_steal_pointer (&proxy);
invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Invalid VM proxy configuration");
  return NULL;
}

GSocketConnection *
pirwa_proxy_connect (const PirwaProxy *proxy, const char *host, guint16 port,
                     GCancellable *cancellable, GError **error)
{
  const char *uri = port == 443 && proxy->https_uri && *proxy->https_uri ? proxy->https_uri
                                                                         : proxy->default_uri;
  g_autoptr (GProxyResolver) resolver = g_simple_proxy_resolver_new (uri, proxy->ignore_hosts);
  g_autoptr (GSocketClient) client = g_socket_client_new ();
  g_socket_client_set_proxy_resolver (client, resolver);
  g_socket_client_set_timeout (client, 10);
  /* One selected proxy. A failed proxy must never fall back to a direct socket. */
  return g_socket_client_connect_to_host (client, host, port, cancellable, error);
}

void
pirwa_proxy_free (PirwaProxy *proxy)
{
  if (!proxy)
    return;
  g_free (proxy->default_uri);
  g_free (proxy->https_uri);
  g_free (proxy->dns);
  g_strfreev (proxy->ignore_hosts);
  g_free (proxy);
}
