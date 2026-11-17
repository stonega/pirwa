/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-usage.h"
#include "pirwa-libvirt.h"
#include "pirwa-network.h"
#include <string.h>

gboolean
pirwa_usage_cpu_percent (const PirwaUsage *previous, const PirwaUsage *current, double *percent)
{
  if (!previous || !current || !percent || !previous->running || !current->running
      || !previous->has_cpu_time || !current->has_cpu_time
      || current->sampled_at_us <= previous->sampled_at_us
      || current->cpu_time_ns < previous->cpu_time_ns)
    return FALSE;
  *percent = (current->cpu_time_ns - previous->cpu_time_ns) * 100.0
             / ((current->sampled_at_us - previous->sampled_at_us) * 1000.0);
  return TRUE;
}

gboolean
pirwa_usage_network_rates (const PirwaUsage *previous, const PirwaUsage *current,
                           double *rx_bytes_per_second, double *tx_bytes_per_second)
{
  if (!previous || !current || !rx_bytes_per_second || !tx_bytes_per_second || !previous->running
      || !current->running || !previous->has_network || !current->has_network
      || current->sampled_at_us <= previous->sampled_at_us
      || memcmp (previous->network_identity, current->network_identity,
                 sizeof current->network_identity)
             != 0
      || current->network_rx_bytes < previous->network_rx_bytes
      || current->network_tx_bytes < previous->network_tx_bytes)
    return FALSE;
  double seconds = (current->sampled_at_us - previous->sampled_at_us) / (double)G_USEC_PER_SEC;
  *rx_bytes_per_second = (current->network_rx_bytes - previous->network_rx_bytes) / seconds;
  *tx_bytes_per_second = (current->network_tx_bytes - previous->network_tx_bytes) / seconds;
  return TRUE;
}

static void
read_network (const virDomainStatsRecord *record, PirwaUsage *usage)
{
  unsigned int count = 0;
  if (virTypedParamsGetUInt (record->params, record->nparams, "net.count", &count) != 1
      || count > (unsigned int)record->nparams / 3)
    return;
  g_autoptr (GChecksum) identity = g_checksum_new (G_CHECKSUM_SHA256);
  guint64 rx_total = 0, tx_total = 0;
  for (unsigned int i = 0; i < count; i++)
    {
      char field[VIR_TYPED_PARAM_FIELD_LENGTH];
      const char *name = NULL;
      unsigned long long rx = 0, tx = 0;
      g_snprintf (field, sizeof field, "net.%u.name", i);
      if (virTypedParamsGetString (record->params, record->nparams, field, &name) != 1 || !name
          || !*name)
        return;
      g_checksum_update (identity, (const guchar *)name, strlen (name) + 1);
      g_snprintf (field, sizeof field, "net.%u.rx.bytes", i);
      if (virTypedParamsGetULLong (record->params, record->nparams, field, &rx) != 1
          || rx > G_MAXUINT64 - rx_total)
        return;
      g_snprintf (field, sizeof field, "net.%u.tx.bytes", i);
      if (virTypedParamsGetULLong (record->params, record->nparams, field, &tx) != 1
          || tx > G_MAXUINT64 - tx_total)
        return;
      rx_total += rx;
      tx_total += tx;
    }
  gsize length = sizeof usage->network_identity;
  g_checksum_get_digest (identity, usage->network_identity, &length);
  usage->network_rx_bytes = rx_total;
  usage->network_tx_bytes = tx_total;
  usage->has_network = TRUE;
}

void
pirwa_usage_from_stats (const virDomainStatsRecord *record, gint64 sampled_at_us, PirwaUsage *usage)
{
  unsigned long long cpu = 0, rss = 0;
  int state = 0;
  *usage = (PirwaUsage){ .sampled_at_us = sampled_at_us };
  usage->running
      = virTypedParamsGetInt (record->params, record->nparams, "state.state", &state) == 1
        && state == VIR_DOMAIN_RUNNING;
  usage->has_cpu_time
      = virTypedParamsGetULLong (record->params, record->nparams, "cpu.time", &cpu) == 1;
  usage->has_memory_rss
      = virTypedParamsGetULLong (record->params, record->nparams, "balloon.rss", &rss) == 1
        && rss <= G_MAXUINT64 / 1024;
  usage->cpu_time_ns = cpu;
  usage->memory_rss_kib = rss;
  read_network (record, usage);
}

gboolean
pirwa_usage_read (virDomainPtr domain, PirwaUsage *usage, GError **error)
{
  virDomainPtr domains[] = { domain, NULL };
  virDomainStatsRecordPtr *records = NULL;
  int count = virDomainListGetStats (domains,
                                     VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL
                                         | VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_INTERFACE,
                                     &records, VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT);
  if (count < 0)
    {
      pirwa_libvirt_error (error, "Cannot read machine usage");
      if (records)
        virDomainStatsRecordListFree (records);
      return FALSE;
    }
  if (count == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This connection does not report machine usage");
      if (records)
        virDomainStatsRecordListFree (records);
      return FALSE;
    }
  pirwa_usage_from_stats (records[0], g_get_monotonic_time (), usage);
  virDomainStatsRecordListFree (records);
  if (usage->running && !usage->has_network)
    pirwa_network_read_usage (domain, usage);
  return TRUE;
}
