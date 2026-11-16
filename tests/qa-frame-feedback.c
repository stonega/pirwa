/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "qa-frame-feedback.h"

typedef struct
{
  GdkFrameTimings *timings;
  gint64 before_paint, after_paint, damage_received;
} Feedback;

static void
clear_feedback (gpointer data)
{
  Feedback *sample = data;
  g_clear_pointer (&sample->timings, gdk_frame_timings_unref);
}

GArray *
qa_frame_feedback_new (void)
{
  GArray *samples = g_array_new (FALSE, FALSE, sizeof (Feedback));
  g_array_set_clear_func (samples, clear_feedback);
  return samples;
}

void
qa_frame_feedback_record (GArray *samples, GdkFrameClock *clock, gint64 before_paint,
                          gint64 after_paint, gint64 damage_received)
{
  g_return_if_fail (samples->len < 10000);
  GdkFrameTimings *timings = gdk_frame_clock_get_current_timings (clock);
  Feedback sample = { .timings = timings ? gdk_frame_timings_ref (timings) : NULL,
                      .before_paint = before_paint,
                      .after_paint = after_paint,
                      .damage_received = damage_received };
  g_array_append_val (samples, sample);
}

static const char *
frame_result (GdkFrameTimings *timings)
{
#if GTK_CHECK_VERSION(4, 24, 0)
  switch (gdk_frame_timings_get_result (timings))
    {
    case GDK_FRAME_PREPARING:
      return "preparing";
    case GDK_FRAME_SKIPPED:
      return "skipped";
    case GDK_FRAME_EMPTY:
      return "empty";
    case GDK_FRAME_SUBMITTED:
      return "submitted";
    case GDK_FRAME_OUTSTANDING:
      return "outstanding";
    case GDK_FRAME_DISCARDED:
      return "discarded";
    case GDK_FRAME_PRESENTED:
      return "presented";
    }
#else
  (void)timings;
#endif
  return "unknown";
}

gint64
qa_frame_feedback_first_presentation (GArray *samples)
{
  gint64 first = 0;
#if GTK_CHECK_VERSION(4, 24, 0)
  for (guint i = 0; i < samples->len; i++)
    {
      Feedback *sample = &g_array_index (samples, Feedback, i);
      GdkFrameTimings *timings = sample->timings;
      if (!timings || !gdk_frame_timings_get_complete (timings)
          || gdk_frame_timings_get_result (timings) != GDK_FRAME_PRESENTED)
        continue;
      gint64 presented = gdk_frame_timings_get_presentation_time (timings);
      if (presented > 0 && presented >= sample->before_paint && (!first || presented < first))
        first = presented;
    }
#else
  (void)samples;
#endif
  return first;
}

void
qa_frame_feedback_json (GArray *samples, GString *json)
{
  g_string_append (
      json,
      "{\"scope\":\"GDK feedback for measured updates, read after sampling; zero timestamps "
      "are unavailable. Only complete presented results identify accurate compositor feedback. "
      "Virtual monitors do not measure physical presentation.\","
      "\"columns\":[\"frame_counter\",\"before_paint_us\",\"after_paint_us\","
      "\"damage_received_us\",\"complete\",\"presentation_us\",\"refresh_interval_us\","
      "\"result\"],\"samples\":[");
  for (guint i = 0; i < samples->len; i++)
    {
      Feedback *sample = &g_array_index (samples, Feedback, i);
      GdkFrameTimings *timings = sample->timings;
      g_string_append_printf (
          json,
          "%s[%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
          ",%s,%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",\"%s\"]",
          i ? "," : "", timings ? gdk_frame_timings_get_frame_counter (timings) : -1,
          sample->before_paint, sample->after_paint, sample->damage_received,
          timings && gdk_frame_timings_get_complete (timings) ? "true" : "false",
          timings ? gdk_frame_timings_get_presentation_time (timings) : 0,
          timings ? gdk_frame_timings_get_refresh_interval (timings) : 0,
          timings ? frame_result (timings) : "unavailable");
    }
  g_string_append (json, "]}");
}
