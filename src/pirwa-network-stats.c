/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-network-stats.h"
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <unistd.h>

/* Abstract sockets disappear with the helper, including after a crash. The
 * Ethernet listener's port identifies the live proxy interface across app
 * restarts; peer credentials restrict both ends to this desktop user. */
static GSocketAddress *
stats_address (guint16 port)
{
  g_autofree char *name = g_strdup_printf ("pirwa-network-stats-%u-%u", (guint)getuid (), port);
  return g_unix_socket_address_new_with_type (name, -1, G_UNIX_SOCKET_ADDRESS_ABSTRACT);
}

static gboolean
same_user (GSocket *socket)
{
  g_autoptr (GCredentials) credentials = g_socket_get_credentials (socket, NULL);
  return credentials && g_credentials_get_unix_user (credentials, NULL) == getuid ();
}

GSocket *
pirwa_network_stats_listen (guint16 port, PirwaNetworkStats *stats)
{
  *stats = (PirwaNetworkStats){ 0 };
  g_autofree char *id = g_uuid_string_random ();
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, (const guchar *)id, strlen (id));
  gsize size = sizeof stats->identity;
  g_checksum_get_digest (checksum, stats->identity, &size);
  g_autoptr (GSocket) listener = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, 0, NULL);
  g_autoptr (GSocketAddress) address = stats_address (port);
  if (!port || !listener || !g_socket_bind (listener, address, FALSE, NULL)
      || !g_socket_listen (listener, NULL))
    return NULL;
  g_socket_set_blocking (listener, FALSE);
  return g_steal_pointer (&listener);
}

void
pirwa_network_stats_reply (GSocket *listener, const PirwaNetworkStats *stats)
{
  g_autoptr (GSocket) peer = g_socket_accept (listener, NULL, NULL);
  if (!peer || !same_user (peer))
    return;
  g_socket_set_blocking (peer, FALSE);
  guint8 packet[56] = "PIRWAN01";
  memcpy (packet + 8, stats->identity, sizeof stats->identity);
  guint64 rx = GUINT64_TO_BE (stats->rx_bytes), tx = GUINT64_TO_BE (stats->tx_bytes);
  memcpy (packet + 40, &rx, sizeof rx);
  memcpy (packet + 48, &tx, sizeof tx);
  /* One bounded nonblocking write; a partial response is rejected by the reader. */
  g_socket_send (peer, (const char *)packet, sizeof packet, NULL, NULL);
}

gboolean
pirwa_network_stats_read (guint16 port, PirwaNetworkStats *stats)
{
  g_autoptr (GSocket) socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, 0, NULL);
  g_autoptr (GSocketAddress) address = stats_address (port);
  if (!port || !socket)
    return FALSE;
  gint64 deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  g_socket_set_timeout (socket, 1);
  if (!g_socket_connect (socket, address, NULL, NULL) || !same_user (socket))
    return FALSE;
  g_socket_set_blocking (socket, FALSE);
  guint8 packet[56];
  gsize received = 0;
  while (received < sizeof packet)
    {
      gint64 remaining = deadline - g_get_monotonic_time ();
      if (remaining <= 0 || !g_socket_condition_timed_wait (socket, G_IO_IN, remaining, NULL, NULL))
        return FALSE;
      gssize size = g_socket_receive (socket, (char *)packet + received, sizeof packet - received,
                                      NULL, NULL);
      if (size <= 0)
        return FALSE;
      received += size;
    }
  if (memcmp (packet, "PIRWAN01", 8) != 0)
    return FALSE;
  guint64 rx, tx;
  memcpy (&rx, packet + 40, sizeof rx);
  memcpy (&tx, packet + 48, sizeof tx);
  memcpy (stats->identity, packet + 8, sizeof stats->identity);
  stats->rx_bytes = GUINT64_FROM_BE (rx);
  stats->tx_bytes = GUINT64_FROM_BE (tx);
  return TRUE;
}
