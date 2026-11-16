/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-avatar.h"
#include "config.h"
#include "pirwa-icons.h"
#include <adwaita.h>
#include <glib/gi18n.h>

static const PirwaAvatarPreset presets[] = {
  { "generic", N_ ("Friendly"), "#99c1f1" },
  { "cheerful", N_ ("Cheerful"), "#8ff0a4" },
  { "wink", N_ ("Wink"), "#ffa3c2" },
  { "curious", N_ ("Curious"), "#dc8add" },
  { "focused", N_ ("Focused"), "#ffbe6f" },
  { "sleepy", N_ ("Sleepy"), "#c0bfbc" },
  { "surprised", N_ ("Surprised"), "#f9f06b" },
  { "cool", N_ ("Cool"), "#93ddc2" },
  { "mischievous", N_ ("Mischievous"), "#ffb5a8" },
  { "calm", N_ ("Calm"), "#b5e8ce" },
  { "dreamy", N_ ("Dreamy"), "#c4b3ee" },
  { "determined", N_ ("Determined"), "#f8d28c" },
  { "playful", N_ ("Playful"), "#99d8ee" },
  { "thoughtful", N_ ("Thoughtful"), "#c3c9ed" },
  { "shy", N_ ("Shy"), "#efb8d2" },
  { "bright", N_ ("Bright"), "#a7e0dd" },
};

const PirwaAvatarPreset *
pirwa_avatar_presets (gsize *length)
{
  *length = G_N_ELEMENTS (presets);
  return presets;
}

const PirwaAvatarPreset *
pirwa_avatar_lookup (const char *id)
{
  for (gsize i = 0; i < G_N_ELEMENTS (presets); i++)
    if (g_strcmp0 (presets[i].id, id) == 0)
      return &presets[i];
  return &presets[0];
}

static void
ensure_icons (void)
{
  static gsize registered;
  if (g_once_init_enter (&registered))
    {
      pirwa_icons_register_resource ();
      g_once_init_leave (&registered, 1);
    }
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  if (!g_object_get_data (G_OBJECT (theme), "pirwa-avatar-icons"))
    {
      gtk_icon_theme_add_resource_path (theme, "/io/github/stonega/Pirwa/avatars");
      g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
      g_autoptr (GString) css
          = g_string_new (".pirwa-avatar.running .pirwa-avatar-features { color: #202b3a; }");
      for (gsize i = 0; i < G_N_ELEMENTS (presets); i++)
        g_string_append_printf (css, ".pirwa-avatar-screen.%s { color: %s; }", presets[i].id,
                                presets[i].screen_color);
      gtk_css_provider_load_from_string (provider, css->str);
      gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_set_data (G_OBJECT (theme), "pirwa-avatar-icons", GINT_TO_POINTER (1));
    }
}

void
pirwa_avatar_set_preset (GtkWidget *avatar, const char *id)
{
  const PirwaAvatarPreset *preset = pirwa_avatar_lookup (id);
  const char *previous = g_object_get_data (G_OBJECT (avatar), "avatar-id");
  if (g_strcmp0 (previous, preset->id) == 0)
    return;
  GtkWidget *screen = g_object_get_data (G_OBJECT (avatar), "screen");
  if (previous)
    gtk_widget_remove_css_class (screen, previous);
  gtk_widget_add_css_class (screen, preset->id);
  g_autofree char *icon = g_strdup_printf ("pirwa-avatar-%s-symbolic", preset->id);
  gtk_image_set_from_icon_name (g_object_get_data (G_OBJECT (avatar), "logo"), icon);
  g_object_set_data (G_OBJECT (avatar), "avatar-id", (gpointer)preset->id);
}

const char *
pirwa_avatar_get_preset (GtkWidget *avatar)
{
  return g_object_get_data (G_OBJECT (avatar), "avatar-id");
}

void
pirwa_avatar_set_running (GtkWidget *avatar, gboolean running)
{
  if (running)
    gtk_widget_add_css_class (avatar, "running");
  else
    gtk_widget_remove_css_class (avatar, "running");
  gtk_widget_set_opacity (g_object_get_data (G_OBJECT (avatar), "screen"), running ? 1 : 0);
}

GtkWidget *
pirwa_avatar_new (const char *id, int size)
{
  ensure_icons ();
  GtkWidget *avatar = gtk_overlay_new ();
  gtk_widget_add_css_class (avatar, "pirwa-avatar");
  gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  GtkWidget *screen = gtk_image_new_from_icon_name ("pirwa-avatar-screen-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (screen), size);
  gtk_widget_add_css_class (screen, "pirwa-avatar-screen");
  /* Opacity reserves the same layout while the powered-off screen is clear. */
  gtk_widget_set_opacity (screen, 0);
  gtk_overlay_set_child (GTK_OVERLAY (avatar), screen);
  g_object_set_data (G_OBJECT (avatar), "screen", screen);
  GtkWidget *frame = gtk_image_new_from_icon_name ("pirwa-avatar-frame-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (frame), size);
  gtk_widget_set_halign (frame, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (frame, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay (GTK_OVERLAY (avatar), frame);
  GtkWidget *logo = gtk_image_new ();
  gtk_widget_add_css_class (logo, "pirwa-avatar-features");
  gtk_image_set_pixel_size (GTK_IMAGE (logo), MAX (1, size * 3 / 4));
  gtk_widget_set_halign (logo, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (logo, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (logo, size * 11 / 64);
  gtk_overlay_add_overlay (GTK_OVERLAY (avatar), logo);
  g_object_set_data (G_OBJECT (avatar), "logo", logo);
  pirwa_avatar_set_preset (avatar, id);
  return avatar;
}

static gboolean
filter_preset (GtkFlowBoxChild *child, gpointer data)
{
  GtkEditable *search = data;
  const char *id = g_object_get_data (G_OBJECT (child), "avatar-id");
  const PirwaAvatarPreset *preset = pirwa_avatar_lookup (id);
  g_autofree char *query = g_utf8_casefold (gtk_editable_get_text (search), -1);
  g_autofree char *name = g_utf8_casefold (_ (preset->name), -1);
  return strstr (name, query) != NULL || strstr (preset->id, query) != NULL;
}

static void
search_changed (GtkSearchEntry *search, GtkFlowBox *flow)
{
  (void)search;
  gtk_flow_box_invalidate_filter (flow);
}

static void
preset_activated (GtkFlowBox *flow, GtkFlowBoxChild *child, AdwExpanderRow *picker)
{
  (void)flow;
  const char *id = g_object_get_data (G_OBJECT (child), "avatar-id");
  const PirwaAvatarPreset *preset = pirwa_avatar_lookup (id);
  g_object_set_data (G_OBJECT (picker), "avatar-id", (gpointer)preset->id);
  pirwa_avatar_set_preset (g_object_get_data (G_OBJECT (picker), "preview"), preset->id);
  adw_expander_row_set_subtitle (picker, _ (preset->name));
  adw_expander_row_set_expanded (picker, FALSE);
}

const char *
pirwa_avatar_picker_get_preset (GtkWidget *picker)
{
  return g_object_get_data (G_OBJECT (picker), "avatar-id");
}

static void
picker_expanded (AdwExpanderRow *picker, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  if (!adw_expander_row_get_expanded (picker) || g_object_get_data (G_OBJECT (picker), "gallery"))
    return;
  const PirwaAvatarPreset *preset
      = pirwa_avatar_lookup (pirwa_avatar_picker_get_preset (GTK_WIDGET (picker)));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  GtkWidget *search = gtk_search_entry_new ();
  g_object_set (search, "placeholder-text", _ ("Search icons"), NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (search), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Search icons"), -1);
  gtk_box_append (GTK_BOX (box), search);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (scroll), 300);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroll), 300);
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_SINGLE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 3);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 3);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_filter_func (GTK_FLOW_BOX (flow), filter_preset, search, NULL);
  for (gsize i = 0; i < G_N_ELEMENTS (presets); i++)
    {
      GtkWidget *child = gtk_flow_box_child_new ();
      GtkWidget *tile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      GtkWidget *name = gtk_label_new (_ (presets[i].name));
      gtk_label_set_max_width_chars (GTK_LABEL (name), 10);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      gtk_widget_set_margin_top (tile, 6);
      gtk_widget_set_margin_bottom (tile, 6);
      gtk_box_append (GTK_BOX (tile), pirwa_avatar_new (presets[i].id, 40));
      gtk_box_append (GTK_BOX (tile), name);
      gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), tile);
      gtk_widget_set_tooltip_text (child, _ (presets[i].name));
      gtk_accessible_update_property (GTK_ACCESSIBLE (child), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _ (presets[i].name), -1);
      g_object_set_data (G_OBJECT (child), "avatar-id", (gpointer)presets[i].id);
      gtk_flow_box_append (GTK_FLOW_BOX (flow), child);
      if (g_str_equal (preset->id, presets[i].id))
        gtk_flow_box_select_child (GTK_FLOW_BOX (flow), GTK_FLOW_BOX_CHILD (child));
    }
  g_signal_connect (search, "search-changed", G_CALLBACK (search_changed), flow);
  g_signal_connect (flow, "child-activated", G_CALLBACK (preset_activated), picker);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), flow);
  gtk_box_append (GTK_BOX (box), scroll);
  adw_expander_row_add_row (ADW_EXPANDER_ROW (picker), box);
  g_object_set_data (G_OBJECT (picker), "gallery", box);
}

GtkWidget *
pirwa_avatar_picker_new (const char *id)
{
  const PirwaAvatarPreset *preset = pirwa_avatar_lookup (id);
  GtkWidget *picker = adw_expander_row_new ();
  gtk_widget_set_name (picker, "machine-avatar-picker");
  gtk_widget_set_tooltip_text (picker, _ ("Choose a machine icon"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Choose a machine icon"), -1);
  GtkWidget *preview = pirwa_avatar_new (preset->id, 48);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (picker), _ ("Choose Icon"));
  adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (picker), _ (preset->name));
  adw_expander_row_add_prefix (ADW_EXPANDER_ROW (picker), preview);
  g_object_set_data (G_OBJECT (picker), "avatar-id", (gpointer)preset->id);
  g_object_set_data (G_OBJECT (picker), "preview", preview);
  g_signal_connect (picker, "notify::expanded", G_CALLBACK (picker_expanded), NULL);
  return picker;
}
