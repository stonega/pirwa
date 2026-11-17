/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

typedef struct
{
  guint8 identity[32]; /* New baseline for each gateway process. */
  guint64 rx_bytes;    /* Ethernet bytes queued to the guest, without socket framing. */
  guint64 tx_bytes;    /* Ethernet bytes received from the guest. */
} PirwaNetworkStats;

/* Optional, same-user local statistics channel. The gateway never waits for readers. */
GSocket *pirwa_network_stats_listen (guint16 port, PirwaNetworkStats *stats);
void pirwa_network_stats_reply (GSocket *listener, const PirwaNetworkStats *stats);
gboolean pirwa_network_stats_read (guint16 port, PirwaNetworkStats *stats);
