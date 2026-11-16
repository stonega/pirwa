/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-network.h"
#include "config.h"
#include "pirwa-libvirt.h"
#include "pirwa-preferences.h"
#include "pirwa-proxy.h"
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static gboolean
plain_source (xmlNode *nic, const char *attribute)
{
  xmlNode *source = NULL;
  for (xmlNode *node = nic->children; node; node = node->next)
    if (xmlStrEqual (node->name, BAD_CAST "source"))
      {
        if (source || !attribute)
          return FALSE;
        source = node;
      }
  if (!source)
    return attribute == NULL;
  if (!source->properties || source->properties->next || source->properties->ns
      || !xmlStrEqual (source->properties->name, BAD_CAST attribute))
    return FALSE;
  for (xmlNode *child = source->children; child; child = child->next)
    if (child->type == XML_ELEMENT_NODE)
      return FALSE;
  xmlChar *value = xmlGetProp (source, BAD_CAST attribute);
  gboolean valid = value && *value;
  xmlFree (value);
  return valid;
}

char *
pirwa_network_domain (const char *xml, guint16 port, GError **error)
{
  xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc || doc->intSubset || doc->extSubset || !port)
    {
      if (doc)
        xmlFreeDoc (doc);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid VM network XML");
      return NULL;
    }
  xmlXPathContext *context = xmlXPathNewContext (doc);
  xmlXPathObject *interfaces
      = xmlXPathEvalExpression (BAD_CAST "/domain/devices/interface", context);
  xmlNode *nic = interfaces && interfaces->nodesetval && interfaces->nodesetval->nodeNr == 1
                     ? interfaces->nodesetval->nodeTab[0]
                     : NULL;
  xmlChar *type = nic ? xmlGetProp (nic, BAD_CAST "type") : NULL;
  gboolean supported
      = type
        && ((xmlStrEqual (type, BAD_CAST "user") && plain_source (nic, NULL))
            || (xmlStrEqual (type, BAD_CAST "bridge") && plain_source (nic, "bridge"))
            || (xmlStrEqual (type, BAD_CAST "network") && plain_source (nic, "network")));
  xmlFree (type);
  xmlXPathObject *custom = xmlXPathEvalExpression (
      BAD_CAST "/domain/devices/interface/ip | "
               "/domain/devices/interface/route | /domain/devices/interface/portForward | "
               "/domain/devices/interface/filterref | /domain/devices/interface/bandwidth | "
               "/domain/devices/interface/virtualport | /domain/devices/interface/vlan | "
               "/domain/devices/interface/teaming | /domain/devices/interface/driver | "
               "/domain/devices/interface/script | /domain/devices/interface/downscript",
      context);
  if (custom && custom->nodesetval && custom->nodesetval->nodeNr)
    supported = FALSE;
  if (custom)
    xmlXPathFreeObject (custom);
  char *result = NULL;
  if (!supported)
    g_set_error_literal (
        error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "This machine's network configuration cannot use the host proxy. "
        "Proxy routing supports one userspace, bridge or virtual network adapter without "
        "custom routes, filters or forwarding. "
        "To start with its usual network, turn off Use host proxy in Preferences → Network.");
  else
    {
      xmlSetProp (nic, BAD_CAST "type", BAD_CAST "client");
      /* Keep MAC, model, guest PCI address, link state and device alias. Host
       * backend options have no meaning on a socket-backed runtime interface. */
      for (xmlNode *node = nic->children; node;)
        {
          xmlNode *next = node->next;
          const char *remove[] = { "backend",     "source",    "target",    "ip", "route",
                                   "portForward", "filterref", "bandwidth", "mtu" };
          for (guint i = 0; i < G_N_ELEMENTS (remove); i++)
            if (xmlStrEqual (node->name, BAD_CAST remove[i]))
              {
                xmlUnlinkNode (node);
                xmlFreeNode (node);
                break;
              }
          node = next;
        }
      xmlNode *source = xmlNewChild (nic, NULL, BAD_CAST "source", NULL);
      g_autofree char *number = g_strdup_printf ("%u", port);
      xmlSetProp (source, BAD_CAST "address", BAD_CAST "127.0.0.1");
      xmlSetProp (source, BAD_CAST "port", BAD_CAST number);
      xmlChar *buffer = NULL;
      int size;
      xmlDocDumpMemory (doc, &buffer, &size);
      result = g_strndup ((const char *)buffer, size);
      xmlFree (buffer);
    }
  if (interfaces)
    xmlXPathFreeObject (interfaces);
  xmlXPathFreeContext (context);
  xmlFreeDoc (doc);
  return result;
}

static GSubprocess *
start_gateway (GSocket *listener, const PirwaProxy *proxy, GCancellable *cancel, GError **error)
{
  int sockets[2];
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot open VM gateway control");
      return NULL;
    }
  g_autoptr (GSocket) socket = g_socket_new_from_fd (sockets[0], error);
  if (!socket)
    {
      close (sockets[0]);
      close (sockets[1]);
      return NULL;
    }
  g_socket_set_timeout (socket, 10);
  g_autoptr (GSocketConnection) control = g_socket_connection_factory_create_connection (socket);
  g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  int listener_fd = dup (g_socket_get_fd (listener));
  if (listener_fd < 0)
    {
      close (sockets[1]);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot pass VM gateway socket");
      return NULL;
    }
  g_subprocess_launcher_take_fd (launcher, listener_fd, 3);
  g_subprocess_launcher_take_fd (launcher, sockets[1], 4);
  const char *helper = g_file_test (PIRWA_NETWORK_BUILD, G_FILE_TEST_IS_EXECUTABLE)
                           ? PIRWA_NETWORK_BUILD
                           : PIRWA_NETWORK_INSTALLED;
  g_autoptr (GSubprocess) child = g_subprocess_launcher_spawn (launcher, error, helper, NULL);
  if (!child)
    return NULL;
  g_autofree char *config = pirwa_proxy_serialize (proxy);
  gsize size = strlen (config);
  guint32 length = GUINT32_TO_BE (size);
  GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (control));
  GInputStream *input = g_io_stream_get_input_stream (G_IO_STREAM (control));
  char ready = 0;
  gsize received = 0;
  gboolean ok = size <= 8192 && g_output_stream_write_all (output, &length, 4, NULL, cancel, error)
                && g_output_stream_write_all (output, config, size, NULL, cancel, error)
                && g_input_stream_read_all (input, &ready, 1, &received, cancel, error)
                && received == 1 && ready == '1';
  if (!ok)
    {
      if (error && !*error)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "The VM proxy gateway could not start");
      g_subprocess_send_signal (child, SIGTERM);
      g_subprocess_wait (child, NULL, NULL);
      return NULL;
    }
  return g_steal_pointer (&child);
}

gboolean
pirwa_network_start (virConnectPtr connection, virDomainPtr domain, GCancellable *cancellable,
                     GError **error)
{
  g_autofree char *uri = virConnectGetURI (connection);
  if (!uri)
    {
      pirwa_libvirt_error (error, "Cannot identify the VM connection");
      return FALSE;
    }
  g_autoptr (PirwaProxy) proxy = NULL;
  if (g_strcmp0 (uri, "qemu:///session") == 0)
    {
      g_autoptr (GSettings) preferences = pirwa_preferences_new ();
      if (g_settings_get_boolean (preferences, "use-host-proxy"))
        {
          g_autoptr (GError) proxy_error = NULL;
          proxy = pirwa_proxy_read (&proxy_error);
          if (proxy_error)
            {
              g_propagate_error (error, g_steal_pointer (&proxy_error));
              return FALSE;
            }
          if (proxy)
            {
              g_free (proxy->dns);
              proxy->dns = g_settings_get_string (preferences, "proxy-dns-address");
              g_autoptr (GInetAddress) dns = g_inet_address_new_from_string (proxy->dns);
              if (!dns || g_inet_address_get_family (dns) != G_SOCKET_FAMILY_IPV4)
                {
                  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       "The VM proxy DNS resolver must be an IPv4 address");
                  return FALSE;
                }
            }
        }
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;
  if (!proxy)
    {
      if (virDomainCreate (domain) == 0)
        return TRUE;
      pirwa_libvirt_error (error, "Cannot start virtual machine");
      return FALSE;
    }
  int saved = virDomainHasManagedSaveImage (domain, 0);
  if (saved != 0)
    {
      if (saved < 0)
        pirwa_libvirt_error (error, "Cannot check saved VM state");
      else
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Resume saved memory with host proxy routing disabled, then fully "
                             "shut down the machine before enabling proxy routing");
      return FALSE;
    }
  g_autofree char *original = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  if (!original)
    {
      pirwa_libvirt_error (error, "Cannot read VM networking");
      return FALSE;
    }
  g_autoptr (GSocket) listener
      = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, 0, error);
  g_autoptr (GInetAddress) loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  g_autoptr (GSocketAddress) address = g_inet_socket_address_new (loopback, 0);
  if (!listener || !g_socket_bind (listener, address, FALSE, error)
      || !g_socket_listen (listener, error))
    return FALSE;
  g_autoptr (GSocketAddress) local = g_socket_get_local_address (listener, error);
  if (!local)
    return FALSE;
  g_autofree char *xml = pirwa_network_domain (
      original, g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (local)), error);
  if (!xml)
    return FALSE;
  g_autoptr (GSubprocess) child = start_gateway (listener, proxy, cancellable, error);
  if (!child)
    return FALSE;
  virDomainPtr running = NULL;
  if (!g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      /* Libvirt uses this as the live definition of the existing inactive UUID
       * and retains its persistent XML. No discovery-time or saved XML edits. */
      running = virDomainCreateXML (connection, xml, 0);
      if (!running)
        pirwa_libvirt_error (error, "Cannot start virtual machine with the host proxy");
    }
  if (!running)
    {
      g_subprocess_send_signal (child, SIGTERM);
      g_subprocess_wait (child, NULL, NULL);
      return FALSE;
    }
  virDomainFree (running);
  return TRUE;
}
