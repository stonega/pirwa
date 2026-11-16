/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-preferences.h"
#include "config.h"

GSettings *
pirwa_preferences_new (void)
{
  GSettingsSchemaSource *system = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema
      = system ? g_settings_schema_source_lookup (system, PIRWA_APPLICATION_ID, TRUE) : NULL;
  /* An older installed schema must not hide keys in a development build. */
  if (schema
      && (!g_settings_schema_has_key (schema, "use-host-proxy")
          || !g_settings_schema_has_key (schema, "selected-machines")
          || !g_settings_schema_has_key (schema, "machine-avatars")))
    g_clear_pointer (&schema, g_settings_schema_unref);
  if (!schema)
    {
      /* Development binaries also work before the application is installed. */
      g_autoptr (GError) error = NULL;
      g_autoptr (GSettingsSchemaSource) local
          = g_settings_schema_source_new_from_directory (PIRWA_SCHEMA_DIR, system, FALSE, &error);
      if (!local)
        g_error ("Cannot load Pirwa preferences: %s", error->message);
      schema = g_settings_schema_source_lookup (local, PIRWA_APPLICATION_ID, FALSE);
    }
  if (!schema)
    g_error ("Pirwa preferences schema is missing");
  return g_settings_new_full (schema, NULL, NULL);
}

char *
pirwa_preferences_get_selected_machine (GSettings *settings, const char *uri)
{
  g_autoptr (GVariant) selections = g_settings_get_value (settings, "selected-machines");
  char *uuid = NULL;
  g_variant_lookup (selections, uri, "s", &uuid);
  return uuid;
}

gboolean
pirwa_preferences_set_selected_machine (GSettings *settings, const char *uri, const char *uuid)
{
  g_autoptr (GVariant) selections = g_settings_get_value (settings, "selected-machines");
  const char *previous = NULL;
  g_variant_lookup (selections, uri, "&s", &previous);
  if (g_strcmp0 (previous, uuid) == 0)
    return TRUE;

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
  GVariantIter iter;
  const char *connection, *selected;
  g_variant_iter_init (&iter, selections);
  while (g_variant_iter_next (&iter, "{&s&s}", &connection, &selected))
    if (!g_str_equal (connection, uri))
      g_variant_builder_add (&builder, "{ss}", connection, selected);
  g_variant_builder_add (&builder, "{ss}", uri, uuid);
  return g_settings_set_value (settings, "selected-machines", g_variant_builder_end (&builder));
}

char *
pirwa_preferences_get_machine_avatar (GSettings *settings, const char *uri, const char *uuid)
{
  g_autoptr (GVariant) avatars = g_settings_get_value (settings, "machine-avatars");
  g_autoptr (GVariant) machines = g_variant_lookup_value (avatars, uri, G_VARIANT_TYPE ("a{ss}"));
  char *preset = NULL;
  if (machines)
    g_variant_lookup (machines, uuid, "s", &preset);
  return preset;
}

gboolean
pirwa_preferences_set_machine_avatar (GSettings *settings, const char *uri, const char *uuid,
                                      const char *preset)
{
  g_return_val_if_fail (uri && *uri && uuid && *uuid, FALSE);
  if (g_strcmp0 (preset, "generic") == 0)
    preset = NULL;
  g_autofree char *previous = pirwa_preferences_get_machine_avatar (settings, uri, uuid);
  if (g_strcmp0 (previous, preset) == 0)
    return TRUE;
  g_autoptr (GVariant) avatars = g_settings_get_value (settings, "machine-avatars");
  g_autoptr (GVariant) machines = g_variant_lookup_value (avatars, uri, G_VARIANT_TYPE ("a{ss}"));
  GVariantBuilder outer, inner;
  g_variant_builder_init (&outer, G_VARIANT_TYPE ("a{sa{ss}}"));
  g_variant_builder_init (&inner, G_VARIANT_TYPE ("a{ss}"));
  GVariantIter iter;
  const char *machine_uuid, *choice;
  if (machines)
    {
      g_variant_iter_init (&iter, machines);
      while (g_variant_iter_next (&iter, "{&s&s}", &machine_uuid, &choice))
        if (!g_str_equal (machine_uuid, uuid))
          g_variant_builder_add (&inner, "{ss}", machine_uuid, choice);
    }
  if (preset)
    g_variant_builder_add (&inner, "{ss}", uuid, preset);
  g_autoptr (GVariant) updated = g_variant_ref_sink (g_variant_builder_end (&inner));
  const char *connection;
  GVariant *existing;
  g_variant_iter_init (&iter, avatars);
  while (g_variant_iter_next (&iter, "{&s@a{ss}}", &connection, &existing))
    {
      if (!g_str_equal (connection, uri))
        g_variant_builder_add (&outer, "{s@a{ss}}", connection, existing);
      g_variant_unref (existing);
    }
  if (g_variant_n_children (updated))
    g_variant_builder_add (&outer, "{s@a{ss}}", uri, updated);
  return g_settings_set_value (settings, "machine-avatars", g_variant_builder_end (&outer));
}
