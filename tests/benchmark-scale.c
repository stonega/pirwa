/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-libvirt.h"
#include <stdlib.h>

static int
compare (const void *a, const void *b)
{
  gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;
  return (x > y) - (x < y);
}

int
main (int argc, char **argv)
{
  guint count = argc > 1 ? g_ascii_strtoull (argv[1], NULL, 10) : 1000;
  if (count > 1000)
    return 2;
  virConnectPtr connection = virConnectOpen ("test:///default");
  if (!connection)
    return 1;
  virDomainPtr *initial = NULL;
  int n = virConnectListAllDomains (connection, &initial, 0);
  for (int i = 0; i < n; i++)
    {
      if (virDomainIsActive (initial[i]) == 1)
        virDomainDestroy (initial[i]);
      virDomainUndefine (initial[i]);
      virDomainFree (initial[i]);
    }
  free (initial);
  for (guint i = 0; i < count; i++)
    {
      g_autofree char *xml
          = g_strdup_printf ("<domain "
                             "type='test'><name>fixture-%04u</name><memory>131072</memory><vcpu>1</"
                             "vcpu><os><type>hvm</type></os></domain>",
                             i);
      virDomainPtr domain = virDomainDefineXML (connection, xml);
      if (!domain)
        {
          virConnectClose (connection);
          return 1;
        }
      virDomainFree (domain);
    }
  gint64 samples[25], sorted[25];
  for (guint i = 0; i < G_N_ELEMENTS (samples); i++)
    {
      g_autoptr (GError) error = NULL;
      gint64 start = g_get_monotonic_time ();
      g_autoptr (GPtrArray) result = pirwa_inventory_read (connection, NULL, &error);
      samples[i] = g_get_monotonic_time () - start;
      if (!result || result->len != count)
        {
          g_printerr ("Inventory failed: %s\n", error ? error->message : "incorrect count");
          virConnectClose (connection);
          return 1;
        }
    }
  memcpy (sorted, samples, sizeof samples);
  qsort (sorted, 25, sizeof (gint64), compare);
  g_print ("{\"driver\":\"test:///"
           "default\",\"method\":\"persistent_bulk\",\"domains\":%u,\"p50_ms\":%.3f,\"p95_ms\":%."
           "3f,\"raw_us\":[",
           count, sorted[12] / 1000.0, sorted[23] / 1000.0);
  for (guint i = 0; i < 25; i++)
    g_print ("%s%" G_GINT64_FORMAT, i ? "," : "", samples[i]);
  g_print ("]}\n");
  virConnectClose (connection);
  return 0;
}
