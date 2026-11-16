/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-preferences.h"

static void
test_machine_avatars (void)
{
  g_autoptr (GSettings) settings = pirwa_preferences_new ();
  g_assert_null (pirwa_preferences_get_machine_avatar (settings, "qemu:///session", "vm-1"));
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-1", "cheerful"));
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-2", "sleepy"));
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///system", "vm-1", "curious"));
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-1", "wink"));
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-1", "wink"));
  g_autoptr (GSettings) reopened = pirwa_preferences_new ();
  g_autofree char *first
      = pirwa_preferences_get_machine_avatar (reopened, "qemu:///session", "vm-1");
  g_autofree char *other
      = pirwa_preferences_get_machine_avatar (reopened, "qemu:///session", "vm-2");
  g_autofree char *system
      = pirwa_preferences_get_machine_avatar (reopened, "qemu:///system", "vm-1");
  g_assert_cmpstr (first, ==, "wink");
  g_assert_cmpstr (other, ==, "sleepy");
  g_assert_cmpstr (system, ==, "curious");
  g_assert_true (
      pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-1", "generic"));
  g_assert_null (pirwa_preferences_get_machine_avatar (reopened, "qemu:///session", "vm-1"));
  g_autofree char *retained
      = pirwa_preferences_get_machine_avatar (reopened, "qemu:///session", "vm-2");
  g_assert_cmpstr (retained, ==, "sleepy");
  g_assert_true (pirwa_preferences_set_machine_avatar (settings, "qemu:///session", "vm-2", NULL));
  g_autoptr (GVariant) avatars = g_settings_get_value (reopened, "machine-avatars");
  g_assert_null (g_variant_lookup_value (avatars, "qemu:///session", G_VARIANT_TYPE ("a{ss}")));
  g_assert_cmpuint (g_variant_n_children (avatars), ==, 1);
  g_assert_true (pirwa_preferences_set_machine_avatar (settings, "qemu:///system", "vm-1", NULL));
}

int
main (int argc, char **argv)
{
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_test_init (&argc, &argv, NULL);
  test_machine_avatars ();
  g_autoptr (GSettings) first = pirwa_preferences_new ();
  g_assert_true (g_settings_get_boolean (first, "use-host-proxy"));
  g_assert_true (g_settings_set_boolean (first, "use-host-proxy", FALSE));
  g_assert_false (g_settings_get_boolean (first, "auto-resize-display"));
  g_assert_true (g_settings_set_boolean (first, "auto-resize-display", TRUE));
  g_autoptr (GSettings) second = pirwa_preferences_new ();
  g_assert_false (g_settings_get_boolean (second, "use-host-proxy"));
  g_assert_true (g_settings_get_boolean (second, "auto-resize-display"));
  g_assert_true (g_settings_set_boolean (second, "auto-resize-display", FALSE));
  g_assert_false (g_settings_get_boolean (first, "auto-resize-display"));

  g_assert_null (pirwa_preferences_get_selected_machine (first, "qemu:///session"));
  g_assert_true (pirwa_preferences_set_selected_machine (first, "qemu:///session", "session-vm"));
  g_assert_true (pirwa_preferences_set_selected_machine (first, "qemu:///system", "system-vm"));
  g_autofree char *session = pirwa_preferences_get_selected_machine (second, "qemu:///session");
  g_autofree char *system = pirwa_preferences_get_selected_machine (second, "qemu:///system");
  g_assert_cmpstr (session, ==, "session-vm");
  g_assert_cmpstr (system, ==, "system-vm");
  g_assert_true (pirwa_preferences_set_selected_machine (second, "qemu:///session", "other-vm"));
  g_assert_true (pirwa_preferences_set_selected_machine (second, "qemu:///session", "other-vm"));
  g_autoptr (GSettings) reopened = pirwa_preferences_new ();
  g_autofree char *restored = pirwa_preferences_get_selected_machine (reopened, "qemu:///session");
  g_assert_cmpstr (restored, ==, "other-vm");
  g_autofree char *unchanged = pirwa_preferences_get_selected_machine (reopened, "qemu:///system");
  g_assert_cmpstr (unchanged, ==, "system-vm");
  g_autoptr (GVariant) selections = g_settings_get_value (reopened, "selected-machines");
  g_assert_cmpuint (g_variant_n_children (selections), ==, 2);

  /* Different schema IDs must still address the pre-release storage location.
   * Use only the memory backend; no desktop preferences are read or changed. */
  g_autoptr (GSettings) legacy = g_settings_new ("io.github.pirwa.Pirwa");
  g_assert_true (g_settings_set_boolean (legacy, "use-host-proxy", TRUE));
  g_assert_true (g_settings_set_boolean (legacy, "auto-resize-display", TRUE));
  g_assert_true (g_settings_set_string (legacy, "proxy-dns-address", "9.9.9.9"));
  g_assert_true (pirwa_preferences_set_selected_machine (legacy, "qemu:///session", "legacy-vm"));
  g_autoptr (GSettings) upgraded = pirwa_preferences_new ();
  g_assert_true (g_settings_get_boolean (upgraded, "use-host-proxy"));
  g_assert_true (g_settings_get_boolean (upgraded, "auto-resize-display"));
  g_autofree char *dns = g_settings_get_string (upgraded, "proxy-dns-address");
  g_assert_cmpstr (dns, ==, "9.9.9.9");
  g_autofree char *selected = pirwa_preferences_get_selected_machine (upgraded, "qemu:///session");
  g_assert_cmpstr (selected, ==, "legacy-vm");
  g_assert_true (g_settings_set_string (upgraded, "proxy-dns-address", "8.8.4.4"));
  g_autofree char *shared_dns = g_settings_get_string (legacy, "proxy-dns-address");
  g_assert_cmpstr (shared_dns, ==, "8.8.4.4");
  return 0;
}
