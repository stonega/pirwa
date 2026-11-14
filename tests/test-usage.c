/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-libvirt.h"

static void
test_cpu_units (void)
{
  PirwaUsage previous = {
    .sampled_at_us = 1000000, .cpu_time_ns = 1000000000, .running = TRUE, .has_cpu_time = TRUE
  };
  PirwaUsage current = previous;
  current.sampled_at_us += 2000000;
  current.cpu_time_ns += 1000000000;
  double percent = 0;
  g_assert_true (pirwa_usage_cpu_percent (&previous, &current, &percent));
  g_assert_cmpfloat_with_epsilon (percent, 50.0, 0.0001);
  current.cpu_time_ns += 3000000000;
  g_assert_true (pirwa_usage_cpu_percent (&previous, &current, &percent));
  g_assert_cmpfloat_with_epsilon (percent, 200.0, 0.0001);
  current.cpu_time_ns = previous.cpu_time_ns;
  g_assert_true (pirwa_usage_cpu_percent (&previous, &current, &percent));
  g_assert_cmpfloat (percent, ==, 0);
}

static void
test_network_rates (void)
{
  PirwaUsage previous = { .sampled_at_us = 1000000,
                          .network_rx_bytes = 1024,
                          .network_tx_bytes = 4096,
                          .running = TRUE,
                          .has_network = TRUE };
  PirwaUsage current = previous;
  current.sampled_at_us += 2000000;
  current.network_rx_bytes += 2048;
  current.network_tx_bytes += 8192;
  double rx = 0, tx = 0;
  g_assert_true (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  g_assert_cmpfloat (rx, ==, 1024);
  g_assert_cmpfloat (tx, ==, 4096);
  current.network_rx_bytes = previous.network_rx_bytes;
  current.network_tx_bytes = previous.network_tx_bytes;
  g_assert_true (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  g_assert_cmpfloat (rx, ==, 0);
  g_assert_cmpfloat (tx, ==, 0);
  previous.network_rx_bytes = G_MAXUINT64 - 4096;
  previous.network_tx_bytes = G_MAXUINT64 - 8192;
  current.network_rx_bytes = G_MAXUINT64 - 2048;
  current.network_tx_bytes = G_MAXUINT64;
  g_assert_true (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  g_assert_cmpfloat (rx, ==, 1024);
  g_assert_cmpfloat (tx, ==, 4096);
}

static void
test_network_baselines (void)
{
  PirwaUsage previous = { .sampled_at_us = 1000000,
                          .network_rx_bytes = 100,
                          .network_tx_bytes = 200,
                          .running = TRUE,
                          .has_network = TRUE };
  PirwaUsage current = previous;
  double rx = 0, tx = 0;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.sampled_at_us--;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.sampled_at_us = previous.sampled_at_us + 2000000;
  current.network_rx_bytes--;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.network_rx_bytes = previous.network_rx_bytes;
  current.network_tx_bytes--;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.network_tx_bytes = previous.network_tx_bytes;
  current.network_identity[0] = 1;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.network_identity[0] = 0;
  current.has_network = FALSE;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.has_network = TRUE;
  previous.has_network = FALSE;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  previous.has_network = TRUE;
  current.running = FALSE;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  current.running = TRUE;
  previous.running = FALSE;
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, &tx));
  previous.running = TRUE;
  g_assert_false (pirwa_usage_network_rates (NULL, &current, &rx, &tx));
  g_assert_false (pirwa_usage_network_rates (&previous, NULL, &rx, &tx));
  g_assert_false (pirwa_usage_network_rates (&previous, &current, NULL, &tx));
  g_assert_false (pirwa_usage_network_rates (&previous, &current, &rx, NULL));
}

static void
test_network_stats (void)
{
  virTypedParameter params[] = {
    { .field = "state.state", .type = VIR_TYPED_PARAM_INT, .value.i = VIR_DOMAIN_RUNNING },
    { .field = "cpu.time", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 1000000000 },
    { .field = "balloon.rss", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 1024 },
    { .field = "net.count", .type = VIR_TYPED_PARAM_UINT, .value.ui = 2 },
    { .field = "net.0.name", .type = VIR_TYPED_PARAM_STRING, .value.s = "vnet0" },
    { .field = "net.0.rx.bytes", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 1024 },
    { .field = "net.0.tx.bytes", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 4096 },
    { .field = "net.1.name", .type = VIR_TYPED_PARAM_STRING, .value.s = "vnet1" },
    { .field = "net.1.rx.bytes", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 2048 },
    { .field = "net.1.tx.bytes", .type = VIR_TYPED_PARAM_ULLONG, .value.ul = 8192 },
  };
  virDomainStatsRecord record = { .params = params, .nparams = G_N_ELEMENTS (params) };
  PirwaUsage first;
  pirwa_usage_from_stats (&record, 1000000, &first);
  g_assert_true (first.running);
  g_assert_true (first.has_cpu_time);
  g_assert_true (first.has_memory_rss);
  g_assert_true (first.has_network);
  g_assert_cmpuint (first.network_rx_bytes, ==, 3072);
  g_assert_cmpuint (first.network_tx_bytes, ==, 12288);
  params[5].value.ul += 2048;
  params[9].value.ul += 8192;
  PirwaUsage second;
  pirwa_usage_from_stats (&record, 3000000, &second);
  double rx = 0, tx = 0;
  g_assert_true (pirwa_usage_network_rates (&first, &second, &rx, &tx));
  g_assert_cmpfloat (rx, ==, 1024);
  g_assert_cmpfloat (tx, ==, 4096);
  /* Replacing an interface needs a new baseline, even if totals increased. */
  params[7].value.s = "vnet2";
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (pirwa_usage_network_rates (&first, &second, &rx, &tx));
  /* A partial sample must not silently undercount the VM's network traffic. */
  record.nparams--;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (second.has_network);
  g_assert_cmpuint (second.network_rx_bytes, ==, 0);
  g_assert_cmpuint (second.network_tx_bytes, ==, 0);
  g_assert_true (second.has_cpu_time);
  g_assert_true (second.has_memory_rss);
  record.nparams++;
  params[5].value.ul = G_MAXUINT64;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (second.has_network); /* Receive total would overflow. */
  params[5].value.ul = 1024;
  params[9].value.ul = G_MAXUINT64;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (second.has_network); /* Transmit total would overflow. */
  params[3].value.ui = G_MAXUINT;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (second.has_network); /* Count must fit the returned record. */
  params[3].value.ui = 0;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_true (second.has_network); /* A VM with no interfaces has zero traffic. */
  g_assert_cmpuint (second.network_rx_bytes, ==, 0);
  g_assert_cmpuint (second.network_tx_bytes, ==, 0);
  record.nparams = 3;
  pirwa_usage_from_stats (&record, 3000000, &second);
  g_assert_false (second.has_network); /* Driver omits all network stats. */
  g_assert_true (second.has_cpu_time);
  g_assert_true (second.has_memory_rss);
}

static void
test_missing_and_reset (void)
{
  PirwaUsage previous = {
    .sampled_at_us = 1000000, .cpu_time_ns = 1000000000, .running = TRUE, .has_cpu_time = TRUE
  };
  PirwaUsage current = previous;
  double percent = 0;
  g_assert_false (pirwa_usage_cpu_percent (&previous, &current, &percent));
  current.sampled_at_us++;
  current.cpu_time_ns = 0; /* A restarted guest has a new counter baseline. */
  g_assert_false (pirwa_usage_cpu_percent (&previous, &current, &percent));
  current.cpu_time_ns = previous.cpu_time_ns + 1;
  current.has_cpu_time = FALSE;
  g_assert_false (pirwa_usage_cpu_percent (&previous, &current, &percent));
  current.has_cpu_time = TRUE;
  previous.running = FALSE;
  g_assert_false (pirwa_usage_cpu_percent (&previous, &current, &percent));
  previous.running = TRUE;
  current.running = FALSE;
  g_assert_false (pirwa_usage_cpu_percent (&previous, &current, &percent));
  g_assert_false (pirwa_usage_cpu_percent (NULL, &current, &percent));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/usage/cpu-units", test_cpu_units);
  g_test_add_func ("/usage/missing-and-reset", test_missing_and_reset);
  g_test_add_func ("/usage/network-rates", test_network_rates);
  g_test_add_func ("/usage/network-baselines", test_network_baselines);
  g_test_add_func ("/usage/network-stats", test_network_stats);
  return g_test_run ();
}
