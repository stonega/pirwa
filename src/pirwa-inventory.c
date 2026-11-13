/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-inventory.h"
#include "config.h"
#include "pirwa-libvirt.h"
#include <glib/gi18n.h>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

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
  virDomainStatsRecordPtr *records = NULL;
  g_autoptr (GPtrArray) machines = g_ptr_array_new_with_free_func (g_object_unref);

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

      g_ptr_array_add (machines, pirwa_machine_new_full (uuid, name, state_label (state), state));
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
