/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-inventory.h"
#include "config.h"
#include "pirwa-config.h"
#include "pirwa-libvirt.h"
#include <glib/gi18n.h>
#include <osinfo/osinfo.h>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

struct _PirwaInventoryCache
{
  GHashTable *os_names;
  OsinfoLoader *loader;
};

PirwaInventoryCache *
pirwa_inventory_cache_new (void)
{
  PirwaInventoryCache *cache = g_new0 (PirwaInventoryCache, 1);
  cache->os_names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return cache;
}

void
pirwa_inventory_cache_free (PirwaInventoryCache *cache)
{
  g_hash_table_unref (cache->os_names);
  g_clear_object (&cache->loader);
  g_free (cache);
}

void
pirwa_inventory_cache_clear (PirwaInventoryCache *cache)
{
  g_hash_table_remove_all (cache->os_names);
}

static const char *
os_name (PirwaInventoryCache *cache, virDomainPtr domain, const char *uuid)
{
  const char *name = g_hash_table_lookup (cache->os_names, uuid);
  if (name)
    return name;
  g_autofree char *xml
      = virDomainGetMetadata (domain, VIR_DOMAIN_METADATA_ELEMENT, PIRWA_OSINFO_NS, 0);
  g_autofree char *id = xml ? pirwa_config_os_id (xml) : NULL;
  if (id)
    {
      if (!cache->loader)
        {
          cache->loader = osinfo_loader_new ();
          osinfo_loader_process_default_path (cache->loader, NULL);
        }
      OsinfoOs *os = osinfo_db_get_os (osinfo_loader_get_db (cache->loader), id);
      if (os)
        name = osinfo_product_get_name (OSINFO_PRODUCT (os));
    }
  /* Cache missing/unknown metadata too, so state refreshes stay bulk queries. */
  char *copy = g_strdup (name ? name : "");
  g_hash_table_insert (cache->os_names, g_strdup (uuid), copy);
  return copy;
}

static const char *
state_label (int state)
{
  switch (state)
    {
    case VIR_DOMAIN_RUNNING:
      return _ ("Running");
    case VIR_DOMAIN_BLOCKED:
      return _ ("Blocked");
    case VIR_DOMAIN_PAUSED:
      return _ ("Paused");
    case VIR_DOMAIN_SHUTDOWN:
      return _ ("Shutting down");
    case VIR_DOMAIN_SHUTOFF:
      return _ ("Powered off");
    case VIR_DOMAIN_CRASHED:
      return _ ("Crashed");
    case VIR_DOMAIN_PMSUSPENDED:
      return _ ("Suspended");
    default:
      return _ ("Unknown");
    }
}

void
pirwa_libvirt_error (GError **error, const char *operation)
{
  const virError *detail = virGetLastError ();
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", operation,
               detail && detail->message ? detail->message : "Unknown libvirt error");
}

static gint
compare_machines (gconstpointer a, gconstpointer b)
{
  PirwaMachine *left = *(PirwaMachine *const *)a;
  PirwaMachine *right = *(PirwaMachine *const *)b;
  int order = g_utf8_collate (pirwa_machine_get_name (left), pirwa_machine_get_name (right));
  return order ? order : g_strcmp0 (pirwa_machine_get_uuid (left), pirwa_machine_get_uuid (right));
}

GPtrArray *
pirwa_inventory_read (virConnectPtr connection, GCancellable *cancellable, GError **error)
{
  return pirwa_inventory_read_cached (connection, NULL, cancellable, error);
}

GPtrArray *
pirwa_inventory_read_cached (virConnectPtr connection, PirwaInventoryCache *cache,
                             GCancellable *cancellable, GError **error)
{
  virDomainStatsRecordPtr *records = NULL;
  g_autoptr (GPtrArray) machines = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GHashTable) seen
      = cache ? g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL) : NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  /* One bulk state query, rather than a round trip for every guest. Name and
   * UUID getters read metadata already held by the returned domain handles. */
  int count = virConnectGetAllDomainStats (connection, VIR_DOMAIN_STATS_STATE, &records, 0);
  if (count < 0)
    {
      pirwa_libvirt_error (error, "Cannot list virtual machines");
      goto cleanup;
    }

  for (int i = 0; i < count; i++)
    {
      char uuid[VIR_UUID_STRING_BUFLEN];
      int state = VIR_DOMAIN_NOSTATE;
      const char *name;

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto cleanup;

      name = virDomainGetName (records[i]->dom);
      if (name == NULL || virDomainGetUUIDString (records[i]->dom, uuid) < 0)
        {
          pirwa_libvirt_error (error, "Cannot read virtual machine identity");
          goto cleanup;
        }

      if (virTypedParamsGetInt (records[i]->params, records[i]->nparams, "state.state", &state) < 0)
        {
          pirwa_libvirt_error (error, "Cannot read virtual machine state");
          goto cleanup;
        }

      PirwaMachine *machine = pirwa_machine_new_full (uuid, name, state_label (state), state);
      if (cache)
        {
          pirwa_machine_set_os_name (machine, os_name (cache, records[i]->dom, uuid));
          g_hash_table_add (seen, g_strdup (uuid));
        }
      g_ptr_array_add (machines, machine);
    }

  if (cache)
    {
      GHashTableIter iter;
      gpointer key;
      g_hash_table_iter_init (&iter, cache->os_names);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        if (!g_hash_table_contains (seen, key))
          g_hash_table_iter_remove (&iter);
    }

  g_ptr_array_sort (machines, compare_machines);

cleanup:
  if (records != NULL)
    virDomainStatsRecordListFree (records);
  return *error ? NULL : g_steal_pointer (&machines);
}

static void
load_worker (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  (void)source_object;
  g_autoptr (GError) error = NULL;
  GPtrArray *machines = NULL;
  if (!g_cancellable_set_error_if_cancelled (cancellable, &error))
    {
      virConnectPtr connection = virConnectOpenReadOnly (task_data);
      if (connection == NULL)
        pirwa_libvirt_error (&error, "Cannot connect to libvirt");
      else
        {
          machines = pirwa_inventory_read (connection, cancellable, &error);
          virConnectClose (connection);
        }
    }

  if (machines != NULL)
    g_task_return_pointer (task, machines, (GDestroyNotify)g_ptr_array_unref);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
pirwa_inventory_load_async (const char *uri, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (uri != NULL && *uri != '\0');
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (uri), g_free);
  g_task_run_in_thread (task, load_worker);
}

GPtrArray *
pirwa_inventory_load_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
