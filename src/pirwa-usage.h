/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

/* Read-only worker snapshot. CPU time is cumulative nanoseconds; RSS is KiB.
 * Network counters are cumulative bytes, summed across the VM's interfaces. */
typedef struct
{
  gint64 sampled_at_us;
  guint64 cpu_time_ns;
  guint64 memory_rss_kib;
  guint64 network_rx_bytes;
  guint64 network_tx_bytes;
  guint8 network_identity[32]; /* SHA-256 of interface names or the proxy gateway's identity. */
  gboolean running;
  gboolean has_cpu_time;
  gboolean has_memory_rss;
  gboolean has_network;
} PirwaUsage;

/* 100% means one host CPU; a multi-core guest can exceed 100%. */
gboolean pirwa_usage_cpu_percent (const PirwaUsage *previous, const PirwaUsage *current,
                                  double *percent);
gboolean pirwa_usage_network_rates (const PirwaUsage *previous, const PirwaUsage *current,
                                    double *rx_bytes_per_second, double *tx_bytes_per_second);
