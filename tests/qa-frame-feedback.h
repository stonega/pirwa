/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gtk/gtk.h>

/* Explicit profiling only. Keeps timing objects alive beyond GDK's history. */
GArray *qa_frame_feedback_new (void);
void qa_frame_feedback_record (GArray *samples, GdkFrameClock *clock, gint64 before_paint,
                               gint64 after_paint, gint64 damage_received);
void qa_frame_feedback_json (GArray *samples, GString *json);
gint64 qa_frame_feedback_first_presentation (GArray *samples);
