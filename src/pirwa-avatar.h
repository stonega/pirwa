/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>

typedef struct
{
  const char *id;
  const char *name;
  const char *screen_color;
} PirwaAvatarPreset;

const PirwaAvatarPreset *pirwa_avatar_presets (gsize *length);
const PirwaAvatarPreset *pirwa_avatar_lookup (const char *id);
GtkWidget *pirwa_avatar_new (const char *id, int size);
void pirwa_avatar_set_preset (GtkWidget *avatar, const char *id);
void pirwa_avatar_set_running (GtkWidget *avatar, gboolean running);
const char *pirwa_avatar_get_preset (GtkWidget *avatar);
GtkWidget *pirwa_avatar_picker_new (const char *id);
const char *pirwa_avatar_picker_get_preset (GtkWidget *picker);
