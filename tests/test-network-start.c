/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-network.h"
#include "pirwa-preferences.h"
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <stdarg.h>

/* Only the URI is changed: every domain operation still uses the test driver.
 * A synthetic QEMU client connects to the real spawned gateway's Ethernet port. */
static const char *uri = "qemu:///session";
static gboolean reject_start;
static guint normal_starts, proxy_starts;
static GSubprocess *gateway;
static GSocketConnection *peer;

char *
__wrap_virConnectGetURI (virConnectPtr connection)
{
  (void)connection;
  return g_strdup (uri);
}

GSubprocess *__real_g_subprocess_launcher_spawn (GSubprocessLauncher *, GError **, const char *,
                                                 ...);
GSubprocess *
__wrap_g_subprocess_launcher_spawn (GSubprocessLauncher *launcher, GError **error,
                                    const char *program, ...)
{
  va_list args;
  va_start (args, program);
  g_assert_null (va_arg (args, const char *));
  va_end (args);
  GSubprocess *child = __real_g_subprocess_launcher_spawn (launcher, error, program, NULL);
  if (child)
    g_set_object (&gateway, child);
  return child;
}

int __real_virDomainCreate (virDomainPtr domain);
int
__wrap_virDomainCreate (virDomainPtr domain)
{
  normal_starts++;
  return __real_virDomainCreate (domain);
}

virDomainPtr __real_virDomainCreateXML (virConnectPtr, const char *, unsigned int);
virDomainPtr
__wrap_virDomainCreateXML (virConnectPtr connection, const char *xml, unsigned int flags)
{
  proxy_starts++;
  if (reject_start)
    return __real_virDomainCreateXML (connection, "not XML", flags);
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, NULL, XML_PARSE_NONET);
  g_assert_nonnull (doc);
  xmlXPathContext *context = xmlXPathNewContext (doc);
  xmlXPathObject *result = xmlXPathEvalExpression (
      BAD_CAST "string(/domain/devices/interface[@type='client']/source/@port)", context);
  g_assert_nonnull (result);
  guint16 port = g_ascii_strtoull ((const char *)result->stringval, NULL, 10);
  g_assert_cmpuint (port, >, 0);
  g_autoptr (GSocketClient) client = g_socket_client_new ();
  g_socket_client_set_enable_proxy (client, FALSE);
  g_socket_client_set_timeout (client, 3);
  g_autoptr (GError) error = NULL;
  peer = g_socket_client_connect_to_host (client, "127.0.0.1", port, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (peer);
  xmlXPathFreeObject (result);
  xmlXPathFreeContext (context);
  xmlFreeDoc (doc);
  return __real_virDomainCreateXML (connection, xml, flags);
}

static void
test_start (gconstpointer data)
{
  const char *mode = data;
  uri = g_str_equal (mode, "uri-error") ? NULL
        : g_str_equal (mode, "system")  ? "qemu:///system"
                                        : "qemu:///session";
  reject_start = g_str_equal (mode, "failed");
  normal_starts = proxy_starts = 0;
  g_autoptr (GSettings) desktop = g_settings_new ("org.gnome.system.proxy");
  g_settings_set_string (desktop, "mode", g_str_equal (mode, "direct") ? "none" : "manual");
  g_autoptr (GSettings) socks = g_settings_new ("org.gnome.system.proxy.socks");
  g_settings_set_string (socks, "host", "127.0.0.1");
  g_settings_set_int (socks, "port", 1); /* No upstream connection is made by these fixtures. */
  g_autoptr (GSettings) preferences = pirwa_preferences_new ();
  g_settings_set_boolean (preferences, "use-host-proxy", !g_str_equal (mode, "disabled"));
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  if (g_str_equal (mode, "cancelled"))
    g_cancellable_cancel (cancel);
  virConnectPtr connection = virConnectOpen ("test:///default");
  g_assert_nonnull (connection);
  const char *nic
      = g_str_equal (mode, "bridge")    ? "<interface type='bridge'><source bridge='virbr0'/>"
                                          "<model type='rtl8139'/></interface>"
        : g_str_equal (mode, "network") ? "<interface type='network'><source network='default'/>"
                                          "<model type='virtio'/></interface>"
        : g_str_equal (mode, "unsupported")
            ? "<interface type='bridge'><source bridge='virbr0'/>"
              "<filterref filter='clean-traffic'/></interface>"
            : "<interface type='user'><model type='virtio'/></interface>";
  g_autofree char *definition = g_strdup_printf (
      "<domain type='test'><name>gateway-start</name><memory>131072</memory><vcpu>1</vcpu>"
      "<os><type>hvm</type></os><devices>%s</devices></domain>",
      nic);
  virDomainPtr domain = virDomainDefineXML (connection, definition);
  g_assert_nonnull (domain);
  g_autofree char *before = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  g_autoptr (GError) error = NULL;
  gboolean success = pirwa_network_start (connection, domain, cancel, &error);
  gboolean no_start = g_str_equal (mode, "cancelled") || g_str_equal (mode, "uri-error")
                      || g_str_equal (mode, "unsupported");
  if (reject_start || no_start)
    {
      g_assert_false (success);
      g_assert_nonnull (error);
      if (g_str_equal (mode, "unsupported"))
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
      g_assert_cmpint (virDomainIsActive (domain), ==, 0);
    }
  else
    {
      g_assert_true (success);
      g_assert_no_error (error);
      g_assert_cmpint (virDomainIsActive (domain), ==, 1);
      g_assert_cmpint (virDomainDestroy (domain), ==, 0);
    }
  gboolean direct = g_str_equal (mode, "direct") || g_str_equal (mode, "disabled")
                    || g_str_equal (mode, "system");
  g_assert_cmpuint (normal_starts, ==, direct ? 1 : 0);
  g_assert_cmpuint (proxy_starts, ==, direct || no_start ? 0 : 1);
  g_assert_true ((gateway != NULL) == (proxy_starts != 0));
  g_autofree char *after = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  g_assert_cmpstr (after, ==, before);
  g_clear_object (&peer);
  if (gateway)
    {
      g_clear_error (&error);
      g_assert_true (g_subprocess_wait_check (gateway, NULL, &error));
      g_assert_no_error (error);
      g_clear_object (&gateway);
    }
  virDomainUndefine (domain);
  virDomainFree (domain);
  virConnectClose (connection);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  const char *modes[] = { "proxy",    "bridge", "network",   "unsupported", "direct",
                          "disabled", "system", "cancelled", "failed",      "uri-error" };
  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      g_autofree char *path = g_strdup_printf ("/network/start/%s", modes[i]);
      g_test_add_data_func (path, modes[i], test_start);
    }
  return g_test_run ();
}
