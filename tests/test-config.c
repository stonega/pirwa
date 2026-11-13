/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-config.h"
#include <glib/gstdio.h>
#include <unistd.h>

static void
test_roundtrip (void)
{
  PirwaCreate options = { .name = "Linux & <test>",
                          .source = "/tmp/install & 'test'.iso",
                          .cpus = 2,
                          .memory_mb = 2048,
                          .disk_gb = 32 };
  g_autoptr (GError) error = NULL;
  g_assert_true (pirwa_config_validate (&options, &error));
  g_autofree char *xml = pirwa_config_domain (
      &options, "11111111-1111-4111-8111-111111111111", "/tmp/disk & test.qcow2",
      "22222222-2222-4222-8222-222222222222", "key&value", TRUE, TRUE, NULL);
  g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (details->name, ==, options.name);
  g_assert_cmpstr (details->disk_path, ==, "/tmp/disk & test.qcow2");
  g_assert_cmpstr (details->volume_key, ==, "key&value");
  g_assert_true (details->managed);
  g_assert_true (details->has_cdrom);
  g_autofree char *edited = pirwa_config_edit (xml, "Renamed & <VM>", 4, 4096, TRUE, &error);
  g_assert_no_error (error);
  g_autoptr (PirwaDetails) changed = pirwa_config_parse (edited, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (changed->name, ==, "Renamed & <VM>");
  g_assert_cmpuint (changed->cpus, ==, 4);
  g_assert_cmpuint (changed->memory_mb, ==, 4096);
  g_assert_false (changed->has_cdrom);
  g_assert_cmpstr (changed->volume_key, ==, details->volume_key);
  g_assert_nonnull (strstr (edited, "spice"));
}

static void
test_validation (void)
{
  PirwaCreate o = { .name = "valid", .cpus = 2, .memory_mb = 2048, .disk_gb = 32 };
  g_autoptr (GError) error = NULL;
  o.name = "../wrong";
  g_assert_false (pirwa_config_validate (&o, &error));
  g_clear_error (&error);
  o.name = "a\nb";
  g_assert_false (pirwa_config_validate (&o, &error));
  g_clear_error (&error);
  o.name = "valid";
  o.cpus = 0;
  g_assert_false (pirwa_config_validate (&o, &error));
  g_clear_error (&error);
  o.cpus = 2;
  o.disk_gb = 4096;
  g_assert_false (pirwa_config_validate (&o, &error));
  g_clear_error (&error);
  g_autoptr (PirwaDetails) invalid
      = pirwa_config_parse ("<!DOCTYPE domain [<!ENTITY x 'x'>]><domain/>", &error);
  g_assert_null (invalid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_qcow2 (void)
{
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  int fd = g_file_open_tmp ("pirwa-qcow2-XXXXXX", &path, &error);
  g_assert_no_error (error);
  guint8 header[104] = { 'Q', 'F', 'I', 0xfb };
  header[7] = 3;
  header[23] = 16;
  header[27] = 2;
  g_assert_cmpint (write (fd, header, sizeof header), ==, sizeof header);
  guint64 capacity;
  g_assert_true (pirwa_qcow2_inspect (fd, &capacity, &error));
  g_assert_cmpuint (capacity, ==, 2ULL << 32);
  header[15] = 1;
  g_assert_cmpint (lseek (fd, 0, SEEK_SET), ==, 0);
  g_assert_cmpint (write (fd, header, sizeof header), ==, sizeof header);
  g_assert_false (pirwa_qcow2_inspect (fd, &capacity, &error));
  g_clear_error (&error);
  header[15] = 0;
  header[79] = 4;
  g_assert_cmpint (lseek (fd, 0, SEEK_SET), ==, 0);
  g_assert_cmpint (write (fd, header, sizeof header), ==, sizeof header);
  g_assert_false (pirwa_qcow2_inspect (fd, &capacity, &error));
  g_clear_error (&error);
  close (fd);
  g_unlink (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/config/roundtrip-and-eject", test_roundtrip);
  g_test_add_func ("/config/validation", test_validation);
  g_test_add_func ("/config/qcow2", test_qcow2);
  return g_test_run ();
}
