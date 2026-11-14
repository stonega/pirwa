/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-boxes.h"
#include "pirwa-libvirt.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct
{
  char *root;
  PirwaBoxesPaths *paths;
} Fixture;

static void
remove_tree (const char *path)
{
  struct stat st;
  if (g_lstat (path, &st) < 0)
    return;
  if (!S_ISDIR (st.st_mode))
    {
      g_unlink (path);
      return;
    }
  g_autoptr (GDir) directory = g_dir_open (path, 0, NULL);
  const char *name;
  while (directory && (name = g_dir_read_name (directory)))
    {
      g_autofree char *child = g_build_filename (path, name, NULL);
      remove_tree (child);
    }
  g_rmdir (path);
}

static void
setup (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  f->root = g_dir_make_tmp ("pirwa-boxes-XXXXXX", &error);
  g_assert_no_error (error);
  f->paths = g_new0 (PirwaBoxesPaths, 1);
  f->paths->native_uri = g_strdup ("test:///default");
  f->paths->state_file = g_build_filename (f->root, "state", "boxes.ini", NULL);
  f->paths->flatpak_root = g_build_filename (f->root, "flatpak", NULL);
  f->paths->runtime_root = g_build_filename (f->root, "runtime", NULL);
  f->paths->flatpak_program = g_build_filename (f->root, "flatpak-info", NULL);
  g_assert_true (
      g_file_set_contents (f->paths->flatpak_program, "#!/bin/sh\nexit 0\n", -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_chmod (f->paths->flatpak_program, 0700), ==, 0);
}

static void
teardown (Fixture *f, gconstpointer data)
{
  (void)data;
  pirwa_boxes_paths_free (f->paths);
  remove_tree (f->root);
  g_free (f->root);
}

static void
write_file (const char *path, const char *contents)
{
  g_autofree char *parent = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
  g_autoptr (GError) error = NULL;
  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
}

static void
test_native (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_false (scan->already_seen);
  g_assert_true (scan->installed);
  g_assert_cmpuint (scan->libraries->len, ==, 0); /* Native domains are already in Session. */
  g_assert_false (g_file_test (f->paths->state_file, G_FILE_TEST_EXISTS));
  /* Discovery does not create definitions, volumes, or an ownership ledger. */
  virConnectPtr connection = virConnectOpenReadOnly ("test:///default");
  virDomainPtr *domains = NULL;
  g_assert_cmpint (virConnectListAllDomains (connection, &domains, 0), ==, 1);
  virDomainFree (domains[0]);
  free (domains);
  virConnectClose (connection);
}

static void
test_not_installed (Fixture *f, gconstpointer data)
{
  (void)data;
  write_file (f->paths->flatpak_program, "#!/bin/sh\nexit 1\n");
  g_autofree char *file
      = g_build_filename (f->paths->flatpak_root, "config", "libvirt", "qemu", "old.xml", NULL);
  write_file (file, "<domain><name>Uninstalled "
                    "Flatpak</name><uuid>11111111-1111-4111-8111-111111111111</uuid></domain>");
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_false (scan->installed);
  g_assert_cmpuint (scan->libraries->len, ==, 0);
  g_assert_false (g_file_test (f->paths->state_file, G_FILE_TEST_EXISTS));
}

static void
test_empty (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *node = g_build_filename (f->root, "empty.xml", NULL);
  write_file (node, "<node/>");
  g_free (f->paths->native_uri);
  f->paths->native_uri = g_strdup_printf ("test://%s", node);
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (scan->libraries->len, ==, 0);
}

static int
make_socket (Fixture *f)
{
  g_autofree char *directory = g_build_filename (f->paths->runtime_root, "libvirt", NULL);
  g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
  g_autofree char *path = g_build_filename (directory, "libvirt-sock", NULL);
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  g_assert_cmpuint (g_strlcpy (address.sun_path, path, sizeof address.sun_path), <,
                    sizeof address.sun_path);
  int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (bind (fd, (struct sockaddr *)&address, sizeof address), ==, 0);
  return fd;
}

static void
test_flatpak (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *directory
      = g_build_filename (f->paths->flatpak_root, "config", "libvirt", "qemu", NULL);
  const char *xml = "<domain type='kvm'><name>Boxes &lt;test&gt;</name>"
                    "<uuid>11111111-1111-4111-8111-111111111111</uuid>"
                    "<metadata><b:gnome-boxes "
                    "xmlns:b='https://wiki.gnome.org/Apps/Boxes'/></metadata></domain>";
  g_autofree char *file = g_build_filename (directory, "machine.xml", NULL);
  write_file (file, xml);
  g_autofree char *duplicate = g_build_filename (directory, "duplicate.xml", NULL);
  write_file (duplicate, xml);
  g_autofree char *unsafe = g_build_filename (directory, "entity.xml", NULL);
  write_file (
      unsafe,
      "<!DOCTYPE domain [<!ENTITY name SYSTEM 'file:///etc/hostname'>]>"
      "<domain><name>&name;</name><uuid>22222222-2222-4222-8222-222222222222</uuid></domain>");
  g_autofree char *alias = g_build_filename (directory, "alias.xml", NULL);
  g_assert_cmpint (symlink (file, alias), ==, 0);
  g_autofree char *fifo = g_build_filename (directory, "pipe.xml", NULL);
  g_assert_cmpint (mkfifo (fifo, 0600), ==, 0);
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, FALSE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (scan->libraries->len, ==, 1);
  PirwaBoxesLibrary *library = g_ptr_array_index (scan->libraries, 0);
  g_assert_false (library->available);
  g_assert_null (library->uri);
  g_assert_cmpuint (library->machines->len, ==, 1);
  g_assert_cmpstr (pirwa_machine_get_name (g_ptr_array_index (library->machines, 0)), ==,
                   "Boxes <test>");
  int fd = make_socket (f);
  g_clear_pointer (&scan, pirwa_boxes_scan_free);
  scan = pirwa_boxes_scan (f->paths, FALSE, NULL, &error);
  g_assert_no_error (error);
  library = g_ptr_array_index (scan->libraries, 0);
  g_assert_true (library->available);
  g_assert_true (g_str_has_prefix (library->uri, "qemu+unix:///session?socket="));
  close (fd);
  g_autofree char *contents = NULL;
  g_assert_true (g_file_get_contents (file, &contents, NULL, &error));
  g_assert_cmpstr (contents, ==, xml);
}

static void
test_remember (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GError) error = NULL;
  int fd = make_socket (f);
  g_assert_true (pirwa_boxes_remember (f->paths, NULL, &error));
  g_assert_no_error (error);
  struct stat st;
  g_assert_cmpint (g_stat (f->paths->state_file, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  /* A completed introduction must not open or rescan libvirt next launch. */
  g_free (f->paths->native_uri);
  f->paths->native_uri = g_strdup ("not-a-libvirt-uri");
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (scan->already_seen);
  g_assert_cmpuint (scan->libraries->len, ==, 0);
  g_assert_null (scan->warning);
  close (fd);
  g_assert_true (pirwa_boxes_remember (f->paths, NULL, &error));
  g_assert_no_error (error);
  g_clear_pointer (&scan, pirwa_boxes_scan_free);
  scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_true (scan->already_seen);
}

static void
test_offline_socket_preference (Fixture *f, gconstpointer data)
{
  (void)data;
  int fd = make_socket (f);
  g_autofree char *old = g_build_filename (f->paths->runtime_root, "libvirt", "libvirt-sock", NULL);
  g_autofree char *modular
      = g_build_filename (f->paths->runtime_root, "libvirt", "virtqemud-sock", NULL);
  g_assert_cmpint (g_rename (old, modular), ==, 0);
  g_autoptr (GError) error = NULL;
  g_assert_true (pirwa_boxes_remember (f->paths, NULL, &error));
  g_assert_no_error (error);
  close (fd);
  g_assert_cmpint (g_unlink (modular), ==, 0);
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (scan->already_seen);
}

static void
test_cancel (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBoxesScan) scan = pirwa_boxes_scan (f->paths, TRUE, cancel, &error);
  g_assert_null (scan);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_assert_false (pirwa_boxes_remember (f->paths, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (g_file_test (f->paths->state_file, G_FILE_TEST_EXISTS));
}

typedef struct
{
  GMainLoop *loop;
  GThread *thread;
  PirwaBoxesScan *scan;
} Async;
static void
loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Async *test = data;
  g_assert_true (g_thread_self () == test->thread);
  g_autoptr (GError) error = NULL;
  test->scan = pirwa_boxes_scan_finish (result, &error);
  g_assert_no_error (error);
  g_main_loop_quit (test->loop);
}

static void
test_async (Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Async test = { .loop = loop, .thread = g_thread_self () };
  pirwa_boxes_scan_async (f->paths, TRUE, NULL, loaded, &test);
  g_clear_pointer (&f->paths, pirwa_boxes_paths_free);
  g_main_loop_run (loop);
  g_assert_cmpuint (test.scan->libraries->len, ==, 0);
  pirwa_boxes_scan_free (test.scan);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/boxes/native-library", Fixture, NULL, setup, test_native, teardown);
  g_test_add ("/boxes/not-installed-with-leftover-data", Fixture, NULL, setup, test_not_installed,
              teardown);
  g_test_add ("/boxes/empty-library", Fixture, NULL, setup, test_empty, teardown);
  g_test_add ("/boxes/flatpak-read-only-discovery", Fixture, NULL, setup, test_flatpak, teardown);
  g_test_add ("/boxes/one-time-preference", Fixture, NULL, setup, test_remember, teardown);
  g_test_add ("/boxes/offline-socket-preference", Fixture, NULL, setup,
              test_offline_socket_preference, teardown);
  g_test_add ("/boxes/cancellation", Fixture, NULL, setup, test_cancel, teardown);
  g_test_add ("/boxes/main-thread-callback", Fixture, NULL, setup, test_async, teardown);
  return g_test_run ();
}
