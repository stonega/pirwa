/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-model.h"

static GPtrArray *
snapshot (const char *first, const char *second, int state)
{
  GPtrArray *items = g_ptr_array_new_with_free_func (g_object_unref);
  if (first)
    g_ptr_array_add (items,
                     pirwa_machine_new_full ("a", first, state == 1 ? "Running" : "Paused", state));
  if (second)
    g_ptr_array_add (items, pirwa_machine_new_full ("b", second, "Powered off", 5));
  return items;
}

static void
changed (GListModel *model, guint position, guint removed, guint added, gpointer data)
{
  (void)model;
  (void)position;
  (void)removed;
  (void)added;
  (*(guint *)data)++;
}

static void
test_order_and_identity (void)
{
  g_autoptr (GListStore) store = g_list_store_new (PIRWA_TYPE_MACHINE);
  guint64 serial = 0;
  guint changes = 0;
  g_signal_connect (store, "items-changed", G_CALLBACK (changed), &changes);
  g_autoptr (GPtrArray) initial = snapshot ("First", "Second", 1);
  g_assert_true (pirwa_model_apply (store, &serial, 1, initial));
  g_autoptr (PirwaMachine) a = g_list_model_get_item (G_LIST_MODEL (store), 0);
  g_autoptr (PirwaMachine) b = g_list_model_get_item (G_LIST_MODEL (store), 1);
  guint structural_changes = changes;
  g_autoptr (GPtrArray) updated = snapshot ("First", "Second", 3);
  g_assert_true (pirwa_model_apply (store, &serial, 3, updated));
  g_assert_cmpint (pirwa_machine_get_state_code (a), ==, 3);
  g_assert_cmpuint (changes, ==, structural_changes);
  /* Old lifecycle data must not undo a newer foreground completion. */
  g_assert_false (pirwa_model_apply (store, &serial, 2, initial));
  g_assert_cmpint (pirwa_machine_get_state_code (a), ==, 3);
  g_assert_cmpuint (serial, ==, 3);
  g_assert_false (pirwa_model_apply (store, &serial, 3, initial));
  g_autoptr (GPtrArray) reordered = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (reordered, pirwa_machine_new_full ("b", "Second", "Powered off", 5));
  g_ptr_array_add (reordered, pirwa_machine_new_full ("a", "ZZ renamed", "Running", 1));
  g_assert_true (pirwa_model_apply (store, &serial, 4, reordered));
  g_autoptr (PirwaMachine) first = g_list_model_get_item (G_LIST_MODEL (store), 0);
  g_autoptr (PirwaMachine) second = g_list_model_get_item (G_LIST_MODEL (store), 1);
  g_assert_true (first == b);
  g_assert_true (second == a);
  g_assert_cmpstr (pirwa_machine_get_name (a), ==, "ZZ renamed");
  g_autoptr (GPtrArray) empty = snapshot (NULL, NULL, 0);
  g_assert_false (pirwa_model_apply (store, &serial, 2, empty));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 2);
  g_assert_true (pirwa_model_apply (store, &serial, 5, empty));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/model/late-delivery-and-stable-identity", test_order_and_identity);
  return g_test_run ();
}
