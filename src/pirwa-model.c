/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-model.h"

gboolean
pirwa_model_apply (GListStore *store, guint64 *last_serial, guint64 serial, GPtrArray *snapshots)
{
  g_return_val_if_fail (G_IS_LIST_STORE (store) && last_serial && snapshots, FALSE);
  if (serial <= *last_serial)
    return FALSE;
  g_autoptr (GHashTable) existing
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  g_autoptr (GPtrArray) ordered = g_ptr_array_new_with_free_func (g_object_unref);
  guint old_count = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < old_count; i++)
    {
      PirwaMachine *machine = g_list_model_get_item (G_LIST_MODEL (store), i);
      g_hash_table_insert (existing, g_strdup (pirwa_machine_get_uuid (machine)), machine);
    }
  for (guint i = 0; i < snapshots->len; i++)
    {
      PirwaMachine *snapshot = g_ptr_array_index (snapshots, i);
      PirwaMachine *machine = g_hash_table_lookup (existing, pirwa_machine_get_uuid (snapshot));
      if (machine)
        pirwa_machine_update (machine, snapshot);
      g_ptr_array_add (ordered, g_object_ref (machine ? machine : snapshot));
    }
  guint prefix = 0, suffix = 0;
  while (prefix < old_count && prefix < ordered->len)
    {
      g_autoptr (PirwaMachine) old = g_list_model_get_item (G_LIST_MODEL (store), prefix);
      if (old != g_ptr_array_index (ordered, prefix))
        break;
      prefix++;
    }
  while (suffix < old_count - prefix && suffix < ordered->len - prefix)
    {
      g_autoptr (PirwaMachine) old
          = g_list_model_get_item (G_LIST_MODEL (store), old_count - suffix - 1);
      if (old != g_ptr_array_index (ordered, ordered->len - suffix - 1))
        break;
      suffix++;
    }
  if (old_count - prefix - suffix || ordered->len - prefix - suffix)
    g_list_store_splice (store, prefix, old_count - prefix - suffix,
                         ordered->pdata ? ordered->pdata + prefix : NULL,
                         ordered->len - prefix - suffix);
  *last_serial = serial;
  return TRUE;
}
