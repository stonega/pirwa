/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-inventory.h"
#include <stdlib.h>

#define SAMPLE_COUNT 25

typedef struct
{
  GMainLoop *loop;
  guint domains;
  GError *error;
} Benchmark;

static void
loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  Benchmark *benchmark = user_data;
  g_autoptr (GPtrArray) machines = pirwa_inventory_load_finish (result, &benchmark->error);
  if (machines != NULL)
    benchmark->domains = machines->len;
  g_main_loop_quit (benchmark->loop);
}

static gint
compare_samples (gconstpointer a, gconstpointer b)
{
  gint64 left = *(const gint64 *)a;
  gint64 right = *(const gint64 *)b;
  return (left > right) - (left < right);
}

int
main (void)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Benchmark benchmark = { .loop = loop };
  gint64 samples[SAMPLE_COUNT];

  /* Report the first request separately; warm percentiles exclude it. */
  gint64 start = g_get_monotonic_time ();
  pirwa_inventory_load_async ("test:///default", NULL, loaded, &benchmark);
  g_main_loop_run (loop);
  gint64 first_us = g_get_monotonic_time () - start;

  for (guint i = 0; i < SAMPLE_COUNT && benchmark.error == NULL; i++)
    {
      start = g_get_monotonic_time ();
      pirwa_inventory_load_async ("test:///default", NULL, loaded, &benchmark);
      g_main_loop_run (loop);
      samples[i] = g_get_monotonic_time () - start;
    }

  if (benchmark.error != NULL)
    {
      g_printerr ("Inventory benchmark failed: %s\n", benchmark.error->message);
      g_clear_error (&benchmark.error);
      return 1;
    }

  qsort (samples, SAMPLE_COUNT, sizeof (gint64), compare_samples);
  g_print ("{\"driver\":\"test:///default\",\"domains\":%u,\"warm_samples\":%u,"
           "\"first_request_ms\":%.3f,\"warm_p50_ms\":%.3f,\"warm_p95_ms\":%.3f}\n",
           benchmark.domains, SAMPLE_COUNT, first_us / 1000.0, samples[12] / 1000.0,
           samples[23] / 1000.0);
  return 0;
}
