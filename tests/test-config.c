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
  g_assert_cmpstr (details->media_source, ==, options.source);
  g_assert_nonnull (details->media_target);
  g_assert_nonnull (strstr (details->media_eject_xml, "device=\"cdrom\""));
  g_assert_null (strstr (details->media_eject_xml, "<source"));
  g_autofree char *edited = pirwa_config_edit (xml, "Renamed & <VM>", 4, 4096, TRUE, &error);
  g_assert_no_error (error);
  g_autoptr (PirwaDetails) changed = pirwa_config_parse (edited, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (changed->name, ==, "Renamed & <VM>");
  g_assert_cmpuint (changed->cpus, ==, 4);
  g_assert_cmpuint (changed->memory_mb, ==, 4096);
  g_assert_false (changed->has_cdrom);
  g_assert_null (changed->media_source);
  g_assert_null (changed->media_eject_xml);
  g_assert_cmpstr (changed->volume_key, ==, details->volume_key);
  g_assert_nonnull (strstr (edited, "spice"));
}

static void
test_media (void)
{
  const char *attributes[]
      = { "file='/tmp/Fedora &amp; Test.iso'", "dev='/dev/sr0'",
          "protocol='https' name='images/install.iso'", "pool='images' volume='installer.iso'" };
  const char *sources[]
      = { "/tmp/Fedora & Test.iso", "/dev/sr0", "images/install.iso", "installer.iso" };
  for (guint i = 0; i < G_N_ELEMENTS (sources); i++)
    {
      g_autofree char *xml = g_strdup_printf (
          "<domain><devices>"
          "<disk device='disk'><source file='/keep.qcow2'/></disk>"
          "<disk device='cdrom'><source file=''/><target dev='sda'/></disk>"
          "<disk type='file' device='cdrom'><driver name='qemu' type='raw'/>"
          "<source %s/><target dev='sdb' bus='sata'/><readonly/><alias name='cd0'/></disk>"
          "<disk device='cdrom'><source file='/other.iso'/><target dev='sdc'/></disk>"
          "</devices></domain>",
          attributes[i]);
      g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, NULL);
      g_assert_true (details->has_cdrom);
      g_assert_cmpstr (details->media_source, ==, sources[i]);
      g_assert_cmpstr (details->media_target, ==, "sdb");
      g_assert_nonnull (strstr (details->media_eject_xml, "dev=\"sdb\""));
      g_assert_null (strstr (details->media_eject_xml, "tray="));
      g_assert_nonnull (strstr (details->media_eject_xml, "<readonly"));
      g_assert_nonnull (strstr (details->media_eject_xml, "name=\"cd0\""));
      g_assert_null (strstr (details->media_eject_xml, "<source"));
      g_assert_null (strstr (details->media_eject_xml, "keep.qcow2"));
      g_assert_null (strstr (details->media_eject_xml, "other.iso"));
    }
  g_autoptr (PirwaDetails) empty = pirwa_config_parse (
      "<domain><devices><disk device='cdrom'><target dev='sda'/></disk></devices></domain>", NULL);
  g_assert_false (empty->has_cdrom);
  g_assert_null (empty->media_source);
  g_assert_null (empty->media_eject_xml);
}

static void
test_media_tray (void)
{
  const char *trays[] = { "", " tray='closed'", " tray='open'" };
  const char *expected[]
      = { "not(/disk/target/@tray)", "/disk/target[@tray='closed']", "/disk/target[@tray='open']" };
  for (guint i = 0; i < G_N_ELEMENTS (trays); i++)
    {
      g_autofree char *xml
          = g_strdup_printf ("<domain><devices><disk type='file' device='cdrom'>"
                             "<source file='/images/install.iso'/><target dev='sda' bus='sata'%s/>"
                             "<readonly/></disk></devices></domain>",
                             trays[i]);
      g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, NULL);
      g_assert_nonnull (details);
      /* QEMU rejects a tray change before it can process the source removal. */
      g_assert_true (pirwa_config_supports (details->media_eject_xml, expected[i]));
      g_assert_true (pirwa_config_supports (
          details->media_eject_xml, "/disk[@type='file' and @device='cdrom']"
                                    "[target[@dev='sda' and @bus='sata']][readonly][not(source)]"));
    }
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

static void
test_os_metadata (void)
{
  const char *metadata = "<info:libosinfo xmlns:info='" PIRWA_OSINFO_NS "'>"
                         "<info:os id='http://pirwa.example/os/1'/></info:libosinfo>";
  g_autofree char *domain = g_strdup_printf ("<domain><metadata>%s</metadata></domain>", metadata);
  g_autofree char *fragment_id = pirwa_config_os_id (metadata);
  g_autofree char *domain_id = pirwa_config_os_id (domain);
  g_assert_cmpstr (fragment_id, ==, "http://pirwa.example/os/1");
  g_assert_cmpstr (domain_id, ==, fragment_id);
  g_assert_null (pirwa_config_os_id ("<domain/>"));
  g_assert_null (
      pirwa_config_os_id ("<libosinfo xmlns='urn:wrong'><os id='wrong-namespace'/></libosinfo>"));
  g_autofree char *stripped_id
      = pirwa_config_os_id ("<libosinfo><os id='http://pirwa.example/os/1'/></libosinfo>");
  g_assert_cmpstr (stripped_id, ==, fragment_id);
  g_assert_null (pirwa_config_os_id (
      "<!DOCTYPE libosinfo [<!ENTITY x 'test'>]><libosinfo xmlns='" PIRWA_OSINFO_NS "'/>"));
}

static void
test_graphics_index (void)
{
  g_assert_cmpint (
      pirwa_config_spice_index ("<domain><devices><graphics type='spice'/></devices></domain>"), ==,
      0);
  g_assert_cmpint (
      pirwa_config_spice_index (
          "<domain><devices><graphics type='vnc'/><graphics type='spice'/></devices></domain>"),
      ==, 1);
  g_assert_cmpint (
      pirwa_config_spice_index ("<domain><devices><graphics type='vnc'/></devices></domain>"), ==,
      -1);
  g_assert_cmpint (pirwa_config_spice_index ("<domain/>"), ==, -1);
  g_assert_cmpint (pirwa_config_spice_index ("<!DOCTYPE domain [<!ENTITY x 'spice'>]><domain/>"),
                   ==, -1);
}

static void
test_embedded_graphics (void)
{
  const char *devices[]
      = { "<graphics type='spice'/>", "<graphics type='spice'><gl enable='no'/></graphics>",
          "<graphics type='spice'><gl enable='yes'/></graphics>", "<graphics type='vnc'/>", "" };
  for (guint i = 0; i < G_N_ELEMENTS (devices); i++)
    {
      g_autofree char *xml = g_strdup_printf ("<domain><devices>%s</devices></domain>", devices[i]);
      g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, NULL);
      g_assert_nonnull (details);
      g_assert_cmpint (details->native_console, ==, i < 2);
    }
}

static void
test_linux_firmware (void)
{
  PirwaCreate options = {
    .name = "Omarchy", .source = "/tmp/omarchy.iso", .cpus = 2, .memory_mb = 2048, .disk_gb = 32
  };
  const char *unsigned_boot = "/domain/os[@firmware='efi']/firmware/feature"
                              "[@name='enrolled-keys'][@enabled='no']";
  for (guint import = 0; import < 2; import++)
    for (guint uefi = 0; uefi < 2; uefi++)
      {
        options.import_image = import;
        options.legacy_bios = !uefi;
        g_autofree char *xml
            = pirwa_config_domain (&options, "11111111-1111-4111-8111-111111111111",
                                   "/tmp/linux.qcow2", "pool", "key", uefi, FALSE, NULL);
        g_assert_cmpint (pirwa_config_supports (xml, unsigned_boot), ==, uefi);
        if (!uefi)
          g_assert_true (pirwa_config_supports (
              xml, "not(/domain/os/@firmware) and not(/domain/os/firmware)"));
        /* Firmware capability and paths remain libvirt's choice. */
        g_assert_true (pirwa_config_supports (
            xml, "not(/domain/os/firmware/feature[@name='secure-boot']) and "
                 "not(/domain/os/loader) and not(/domain/os/nvram) and not(/domain/os/varstore)"));
        g_autoptr (GError) error = NULL;
        g_autofree char *edited = pirwa_config_edit (xml, "Renamed Linux", 4, 4096, FALSE, &error);
        g_assert_no_error (error);
        g_assert_cmpint (pirwa_config_supports (edited, unsigned_boot), ==, uefi);
      }
  /* Resource edits must also preserve an existing guest's enrolled keys. */
  g_autofree char *edited = pirwa_config_edit (
      "<domain><name>existing</name><os firmware='efi'><firmware>"
      "<feature name='secure-boot' enabled='yes'/><feature name='enrolled-keys' enabled='yes'/>"
      "</firmware></os></domain>",
      "Existing Linux", 2, 2048, FALSE, NULL);
  g_assert_true (pirwa_config_supports (
      edited, "/domain/os/firmware[feature[@name='secure-boot'][@enabled='yes']]"
              "[feature[@name='enrolled-keys'][@enabled='yes']]"));
}

static void
test_windows (void)
{
  PirwaCreate options = { .name = "Windows",
                          .source = "/tmp/Windows & Setup.iso",
                          .driver_iso = "/tmp/virtio & drivers.iso",
                          .cpus = 2,
                          .memory_mb = 4096,
                          .disk_gb = 64 };
  for (PirwaGuest guest = PIRWA_GUEST_WINDOWS_10; guest <= PIRWA_GUEST_WINDOWS_11; guest++)
    {
      options.guest = guest;
      g_autoptr (GError) error = NULL;
      g_assert_true (pirwa_config_validate (&options, &error));
      g_assert_no_error (error);
      g_autofree char *xml = pirwa_config_domain (
          &options, "11111111-1111-4111-8111-111111111111", "/tmp/windows.qcow2",
          "22222222-2222-4222-8222-222222222222", "disk-key", TRUE, FALSE,
          guest == PIRWA_GUEST_WINDOWS_11 ? "http://microsoft.com/win/11"
                                          : "http://microsoft.com/win/10");
      g_assert_true (pirwa_config_supports (
          xml, "count(/domain/devices/disk[@device='cdrom'])=2 and "
               "/domain/devices/disk[@device='cdrom'][target/@dev='sda']"
               "[source/@file='/tmp/Windows & Setup.iso'][readonly] and "
               "/domain/devices/disk[@device='cdrom'][target/@dev='sdb']"
               "[source/@file='/tmp/virtio & drivers.iso'][readonly] and "
               "/domain/devices/disk[@device='disk']/target[@bus='virtio'] and "
               "/domain/devices/interface/model[@type='virtio'] and "
               "/domain/clock[@offset='localtime'] and /domain/cpu/topology[@sockets='1']"));
      const char *security = "/domain/os[@firmware='efi']/firmware"
                             "[feature[@name='secure-boot' and @enabled='yes']]"
                             "[feature[@name='enrolled-keys' and @enabled='yes']] and "
                             "/domain/features/smm[@state='on'] and "
                             "/domain/devices/tpm[@model='tpm-crb']/backend"
                             "[@type='emulator' and @version='2.0' and @persistent_state='yes']";
      g_assert_cmpint (pirwa_config_supports (xml, security), ==, guest == PIRWA_GUEST_WINDOWS_11);
      g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, &error);
      g_assert_no_error (error);
      g_assert_cmpint (details->has_tpm, ==, guest == PIRWA_GUEST_WINDOWS_11);
      g_assert_cmpstr (details->media_source, ==, options.source);
      g_assert_cmpstr (details->media_target, ==, "sda");
      g_assert_true (details->native_console);
      g_autofree char *edited = pirwa_config_edit (xml, "Renamed Windows", 4, 8192, FALSE, &error);
      g_assert_no_error (error);
      g_assert_cmpint (pirwa_config_supports (edited, security), ==,
                       guest == PIRWA_GUEST_WINDOWS_11);
      g_assert_true (pirwa_config_supports (
          edited, "count(/domain/devices/disk[@device='cdrom'])=2 and "
                  "/domain/devices/disk[@device='disk']/target[@bus='virtio']"));
    }
  /* Linux keeps its device and firmware defaults even if a caller
   * happens to retain a driver path while changing profiles. */
  options.guest = PIRWA_GUEST_LINUX;
  g_autofree char *linux_xml
      = pirwa_config_domain (&options, "11111111-1111-4111-8111-111111111111", "/tmp/linux.qcow2",
                             "pool", "key", TRUE, FALSE, NULL);
  g_assert_true (pirwa_config_supports (
      linux_xml, "count(/domain/devices/disk[@device='cdrom'])=1 and not(/domain/devices/tpm) and "
                 "/domain/os/firmware/feature[@name='enrolled-keys'][@enabled='no'] and "
                 "not(/domain/features/smm)"));
}

static void
test_windows_validation (void)
{
  PirwaCreate o = { .name = "Windows",
                    .cpus = 2,
                    .memory_mb = 4096,
                    .disk_gb = 64,
                    .guest = PIRWA_GUEST_WINDOWS_11,
                    .driver_iso = "/tmp/virtio-win.iso" };
  for (guint i = 0; i < 7; i++)
    {
      PirwaCreate invalid = o;
      switch (i)
        {
        case 0:
          invalid.cpus = 1;
          break;
        case 1:
          invalid.memory_mb = 2048;
          break;
        case 2:
          invalid.disk_gb = 32;
          break;
        case 3:
          invalid.legacy_bios = TRUE;
          break;
        case 4:
          invalid.driver_iso = NULL;
          break;
        case 5:
          invalid.driver_iso = "relative.iso";
          break;
        case 6:
          invalid.import_image = TRUE;
          break;
        }
      g_autoptr (GError) error = NULL;
      g_assert_false (pirwa_config_validate (&invalid, &error));
      g_assert_nonnull (error);
    }
  o.guest = PIRWA_GUEST_WINDOWS_10;
  o.cpus = 1;
  o.memory_mb = 2048;
  o.disk_gb = 32;
  o.legacy_bios = TRUE;
  g_assert_true (pirwa_config_validate (&o, NULL));
}

static void
test_windows_host (void)
{
  PirwaCreate o = { .guest = PIRWA_GUEST_WINDOWS_11 };
  const char *os = "<os><enum name='firmware'><value>efi</value></enum></os>";
  const char *tpm = "<tpm supported='yes'>"
                    "<enum name='model'><value>tpm-crb</value></enum>"
                    "<enum name='backendModel'><value>emulator</value></enum>"
                    "<enum name='backendVersion'><value>2.0</value></enum></tpm>";
  g_autofree char *caps = g_strdup_printf (
      "<domainCapabilities>%s<devices>%s</devices></domainCapabilities>", os, tpm);
  g_assert_true (pirwa_config_check_host (&o, caps, NULL));
  const char *missing[]
      = { "<domainCapabilities/>",
          "<domainCapabilities><os><enum name='firmware'><value>efi</value>"
          "</enum></os><devices><tpm supported='no'/></devices></domainCapabilities>" };
  for (guint i = 0; i < G_N_ELEMENTS (missing); i++)
    {
      g_autoptr (GError) error = NULL;
      g_assert_false (pirwa_config_check_host (&o, missing[i], &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    }
  const char *features[]
      = { "<firmwareFeatures supported='yes'>"
          "<enum name='secureBoot'><value>yes</value></enum>"
          "<enum name='enrolledKeys'><value>yes</value></enum></firmwareFeatures>",
          "<firmwareFeatures supported='yes'>"
          "<enum name='secureBoot'><value>no</value></enum>"
          "<enum name='enrolledKeys'><value>yes</value></enum></firmwareFeatures>",
          "<firmwareFeatures supported='yes'>"
          "<enum name='secureBoot'><value>yes</value></enum>"
          "<enum name='enrolledKeys'><value>no</value></enum></firmwareFeatures>" };
  for (guint i = 0; i < G_N_ELEMENTS (features); i++)
    {
      g_autofree char *modern = g_strdup_printf (
          "<domainCapabilities><os><enum name='firmware'><value>efi</value></enum>%s</os>"
          "<devices>%s</devices></domainCapabilities>",
          features[i], tpm);
      g_autoptr (GError) error = NULL;
      g_assert_cmpint (pirwa_config_check_host (&o, modern, &error), ==, i == 0);
      if (i != 0)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    }
  o.guest = PIRWA_GUEST_WINDOWS_10;
  g_assert_true (pirwa_config_check_host (&o, "<domainCapabilities/>", NULL));
}

static void
test_creation_graphics (void)
{
  PirwaCreate options = {
    .name = "COSMIC", .source = "/tmp/install.iso", .cpus = 2, .memory_mb = 2048, .disk_gb = 32
  };
  for (guint i = 0; i < 2; i++)
    {
      options.import_image = i != 0;
      g_autofree char *xml = pirwa_config_domain (
          &options, "11111111-1111-4111-8111-111111111111", "/tmp/disk.qcow2",
          "22222222-2222-4222-8222-222222222222", "disk-key", TRUE, FALSE, NULL);
      /* COSMIC's software renderer needs QXL rather than unaccelerated virtio.
       * Keep the native console and virtio storage/networking for both paths. */
      const char *graphics = "/domain/devices/video/model[@type='qxl'] and "
                             "/domain/devices/graphics[@type='spice']/listen[@type='socket'] and "
                             "not(/domain/devices/graphics/gl[@enable='yes']) and "
                             "not(/domain/devices/video/model/acceleration[@accel3d='yes'])";
      g_assert_true (pirwa_config_supports (xml, graphics));
      g_assert_true (pirwa_config_supports (
          xml, "/domain/devices/disk[@device='disk']/target[@bus='virtio'] and "
               "/domain/devices/interface/model[@type='virtio']"));
      g_autoptr (PirwaDetails) details = pirwa_config_parse (xml, NULL);
      g_assert_true (details->native_console);
      g_autofree char *edited = pirwa_config_edit (xml, "Renamed", 4, 4096, TRUE, NULL);
      g_assert_true (pirwa_config_supports (edited, graphics));
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/config/os-metadata", test_os_metadata);
  g_test_add_func ("/config/graphics-index", test_graphics_index);
  g_test_add_func ("/config/embedded-graphics", test_embedded_graphics);
  g_test_add_func ("/config/creation-graphics", test_creation_graphics);
  g_test_add_func ("/config/linux-firmware", test_linux_firmware);
  g_test_add_func ("/config/windows", test_windows);
  g_test_add_func ("/config/windows-validation", test_windows_validation);
  g_test_add_func ("/config/windows-host", test_windows_host);
  g_test_add_func ("/config/roundtrip-and-eject", test_roundtrip);
  g_test_add_func ("/config/media", test_media);
  g_test_add_func ("/config/media-tray", test_media_tray);
  g_test_add_func ("/config/validation", test_validation);
  g_test_add_func ("/config/qcow2", test_qcow2);
  return g_test_run ();
}
