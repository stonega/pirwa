/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit headless copy profiler; never a timing assertion in meson test. */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-frame.h"
#include <time.h>

static gint64
thread_cpu (void)
{
  struct timespec value;
  if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &value) != 0)
    g_error ("Cannot read the thread CPU clock");
  return (gint64)value.tv_sec * G_USEC_PER_SEC + value.tv_nsec / 1000;
}

int
main (int argc, char **argv)
{
  guint64 width = 3840, height = 2160, count = 180;
  if (argc != 1
      && (argc != 4 || !g_ascii_string_to_unsigned (argv[1], 10, 64, 8192, &width, NULL)
          || !g_ascii_string_to_unsigned (argv[2], 10, 64, 8192, &height, NULL)
          || !g_ascii_string_to_unsigned (argv[3], 10, 30, 1000, &count, NULL)))
    {
      g_printerr ("Usage: benchmark-frame [WIDTH HEIGHT FRAMES]\n");
      return 2;
    }
  gsize size = (gsize)width * height * 4;
  g_autofree guint8 *pixels = g_malloc (size);
  memset (pixels, 42, size);
  SpiceDisplayPrimary primary = { .format = SPICE_SURFACE_FMT_32_xRGB,
                                  .width = (int)width,
                                  .height = (int)height,
                                  .stride = (int)width * 4,
                                  .data = pixels };
  GdkTexture *retained[3] = { NULL };
  g_autofree gint64 *wall = g_new (gint64, count);
  g_autofree gint64 *cpu = g_new (gint64, count);
  for (guint i = 0; i < count + 30; i++)
    {
      /* Model delayed texture release without GPU work. Destruction stays
       * outside the measured creation span, just as in the console profiler. */
      guint slot = i % G_N_ELEMENTS (retained);
      g_clear_object (&retained[slot]);
      gint64 start = g_get_monotonic_time ();
      gint64 cpu_start = thread_cpu ();
      retained[slot] = pirwa_frame_texture (&primary, NULL, NULL);
      if (!retained[slot])
        g_error ("Frame creation failed");
      gint64 cpu_end = thread_cpu ();
      gint64 end = g_get_monotonic_time ();
      if (i >= 30)
        {
          wall[i - 30] = end - start;
          cpu[i - 30] = cpu_end - cpu_start;
        }
    }
  for (guint i = 0; i < G_N_ELEMENTS (retained); i++)
    g_clear_object (&retained[i]);
  g_print ("{\"width\":%" G_GUINT64_FORMAT ",\"height\":%" G_GUINT64_FORMAT
           ",\"warmup\":30,\"retained_frames\":3,\"sample_columns\":[\"wall_us\",\"cpu_us\"],"
           "\"samples\":[",
           width, height);
  for (guint i = 0; i < count; i++)
    g_print ("%s[%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "]", i ? "," : "", wall[i], cpu[i]);
  g_print ("]}\n");
  return 0;
}
