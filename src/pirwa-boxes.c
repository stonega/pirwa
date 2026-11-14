/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-boxes.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <sys/stat.h>
#include <unistd.h>

PirwaBoxesPaths *
pirwa_boxes_paths_new (const char *native_uri)
{
  PirwaBoxesPaths *paths = g_new0 (PirwaBoxesPaths, 1);
  paths->native_uri = g_strdup (native_uri ? native_uri : "qemu:///session");
  paths->state_file = g_build_filename (g_get_user_state_dir (), "pirwa", "boxes-import.ini", NULL);
  paths->flatpak_root
      = g_build_filename (g_get_home_dir (), ".var", "app", "org.gnome.Boxes", NULL);
  paths->runtime_root
      = g_build_filename (g_get_user_runtime_dir (), "app", "org.gnome.Boxes", NULL);
  paths->flatpak_program = g_strdup ("flatpak");
  return paths;
}

PirwaBoxesPaths *
pirwa_boxes_paths_copy (const PirwaBoxesPaths *paths)
{
  PirwaBoxesPaths *copy = g_new0 (PirwaBoxesPaths, 1);
  copy->native_uri = g_strdup (paths->native_uri);
  copy->state_file = g_strdup (paths->state_file);
  copy->flatpak_root = g_strdup (paths->flatpak_root);
  copy->runtime_root = g_strdup (paths->runtime_root);
  copy->flatpak_program = g_strdup (paths->flatpak_program);
  return copy;
}

void
pirwa_boxes_paths_free (PirwaBoxesPaths *paths)
{
  if (!paths)
    return;
  g_free (paths->native_uri);
  g_free (paths->state_file);
  g_free (paths->flatpak_root);
  g_free (paths->runtime_root);
  g_free (paths->flatpak_program);
  g_free (paths);
}

static void
library_free (gpointer data)
{
  PirwaBoxesLibrary *library = data;
  g_free (library->uri);
  g_ptr_array_unref (library->machines);
  g_free (library);
}

void
pirwa_boxes_scan_free (PirwaBoxesScan *scan)
{
  if (!scan)
    return;
  g_ptr_array_unref (scan->libraries);
  g_free (scan->warning);
  g_free (scan);
}

static PirwaBoxesLibrary *
library_new (const char *uri)
{
  PirwaBoxesLibrary *library = g_new0 (PirwaBoxesLibrary, 1);
  library->uri = g_strdup (uri);
  library->machines = g_ptr_array_new_with_free_func (g_object_unref);
  return library;
}

static char *
flatpak_uri (const PirwaBoxesPaths *paths, gboolean *available)
{
  const char *sockets[] = { "virtqemud-sock", "libvirt-sock" };
  for (guint i = 0; i < G_N_ELEMENTS (sockets); i++)
    {
      g_autofree char *path = g_build_filename (paths->runtime_root, "libvirt", sockets[i], NULL);
      struct stat st;
      if (g_lstat (path, &st) == 0 && S_ISSOCK (st.st_mode) && st.st_uid == getuid ())
        {
          *available = TRUE;
          g_autofree char *escaped = g_uri_escape_string (path, "/", FALSE);
          return g_strdup_printf ("qemu+unix:///session?socket=%s", escaped);
        }
    }
  *available = FALSE;
  return NULL;
}

static gint
compare_machines (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (pirwa_machine_get_name (*(PirwaMachine *const *)a),
                         pirwa_machine_get_name (*(PirwaMachine *const *)b));
}

/* Flatpak configuration is read only. No symlinks, pipes, external entities,
 * directory recursion, or disk image reads during discovery. */
static PirwaMachine *
read_definition (const char *path)
{
  int fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
  if (fd < 0)
    return NULL;
  struct stat st;
  char *contents = NULL;
  if (fstat (fd, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0
      && st.st_size <= 4 * 1024 * 1024)
    {
      contents = g_malloc (st.st_size + 1);
      gsize done = 0;
      while (done < (gsize)st.st_size)
        {
          ssize_t size = read (fd, contents + done, st.st_size - done);
          if (size < 0 && errno == EINTR)
            continue;
          if (size <= 0)
            break;
          done += size;
        }
      contents[done] = '\0';
    }
  close (fd);
  if (!contents)
    return NULL;
  xmlDoc *doc = xmlReadMemory (contents, strlen (contents), NULL, NULL,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  g_free (contents);
  if (!doc)
    return NULL;
  xmlNode *root = xmlDocGetRootElement (doc);
  char *name = NULL, *uuid = NULL;
  if (!doc->intSubset && !doc->extSubset && root && xmlStrEqual (root->name, BAD_CAST "domain"))
    for (xmlNode *node = root->children; node; node = node->next)
      {
        if (node->type != XML_ELEMENT_NODE)
          continue;
        if (xmlStrEqual (node->name, BAD_CAST "name"))
          {
            xmlFree (name);
            name = (char *)xmlNodeGetContent (node);
          }
        if (xmlStrEqual (node->name, BAD_CAST "uuid"))
          {
            xmlFree (uuid);
            uuid = (char *)xmlNodeGetContent (node);
          }
      }
  PirwaMachine *machine = name && *name && uuid && g_uuid_string_is_valid (uuid)
                                  && !g_str_has_prefix (name, "guestfs-")
                              ? pirwa_machine_new (uuid, name, "")
                              : NULL;
  xmlFree (name);
  xmlFree (uuid);
  xmlFreeDoc (doc);
  return machine;
}

PirwaBoxesScan *
pirwa_boxes_scan (const PirwaBoxesPaths *paths, gboolean automatic, GCancellable *cancellable,
                  GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;
  g_autoptr (PirwaBoxesScan) scan = g_new0 (PirwaBoxesScan, 1);
  scan->libraries = g_ptr_array_new_with_free_func (library_free);
  g_autoptr (GKeyFile) state = g_key_file_new ();
  if (g_key_file_load_from_file (state, paths->state_file, G_KEY_FILE_NONE, NULL))
    scan->already_seen = g_key_file_get_boolean (state, "Boxes", "IntroductionSeen", NULL);
  if (scan->already_seen && automatic)
    return g_steal_pointer (&scan);
  /* Check the installed ref, not a native desktop entry or leftover data. */
  g_autoptr (GSubprocess) info
      = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                          NULL, paths->flatpak_program ? paths->flatpak_program : "flatpak", "info",
                          "--show-ref", "org.gnome.Boxes", NULL);
  scan->installed = info && g_subprocess_wait_check (info, cancellable, NULL);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      if (info)
        g_subprocess_force_exit (info);
      return NULL;
    }
  if (!scan->installed)
    return g_steal_pointer (&scan);

  PirwaBoxesLibrary *flatpak = library_new (NULL);
  flatpak->uri = flatpak_uri (paths, &flatpak->available);
  g_autofree char *directory
      = g_build_filename (paths->flatpak_root, "config", "libvirt", "qemu", NULL);
  g_autoptr (GDir) dir = g_dir_open (directory, 0, NULL);
  g_autoptr (GHashTable) uuids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        {
          library_free (flatpak);
          return NULL;
        }
      if (!g_str_has_suffix (name, ".xml"))
        continue;
      g_autofree char *path = g_build_filename (directory, name, NULL);
      g_autoptr (PirwaMachine) machine = read_definition (path);
      if (machine && g_hash_table_add (uuids, g_strdup (pirwa_machine_get_uuid (machine))))
        g_ptr_array_add (flatpak->machines, g_steal_pointer (&machine));
    }
  g_ptr_array_sort (flatpak->machines, compare_machines);
  if (flatpak->machines->len)
    g_ptr_array_add (scan->libraries, flatpak);
  else
    library_free (flatpak);
  return g_steal_pointer (&scan);
}

gboolean
pirwa_boxes_remember (const PirwaBoxesPaths *paths, GCancellable *cancellable, GError **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;
  g_autofree char *directory = g_path_get_dirname (paths->state_file);
  if (g_mkdir_with_parents (directory, 0700) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot save import preference: %s", g_strerror (errno));
      return FALSE;
    }
  g_autoptr (GKeyFile) state = g_key_file_new ();
  g_key_file_load_from_file (state, paths->state_file, G_KEY_FILE_NONE, NULL);
  g_key_file_set_boolean (state, "Boxes", "IntroductionSeen", TRUE);
  g_key_file_remove_key (state, "Boxes", "Flatpak", NULL);
  g_key_file_remove_key (state, "Boxes", "Socket", NULL);
  gsize size;
  g_autofree char *contents = g_key_file_to_data (state, &size, NULL);
  return g_file_set_contents_full (paths->state_file, contents, size,
                                   G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                   0600, error);
}

typedef struct
{
  PirwaBoxesPaths *paths;
  gboolean automatic;
  gboolean remember;
} Work;

static void
work_free (gpointer data)
{
  Work *work = data;
  pirwa_boxes_paths_free (work->paths);
  g_free (work);
}

static void
work_run (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void)source;
  Work *work = data;
  g_autoptr (GError) error = NULL;
  if (work->remember)
    {
      if (pirwa_boxes_remember (work->paths, cancellable, &error))
        g_task_return_boolean (task, TRUE);
      else
        g_task_return_error (task, g_steal_pointer (&error));
    }
  else
    {
      PirwaBoxesScan *scan = pirwa_boxes_scan (work->paths, work->automatic, cancellable, &error);
      if (scan)
        g_task_return_pointer (task, scan, (GDestroyNotify)pirwa_boxes_scan_free);
      else
        g_task_return_error (task, g_steal_pointer (&error));
    }
}

static void
start_work (const PirwaBoxesPaths *paths, gboolean automatic, gboolean remember,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  Work *work = g_new0 (Work, 1);
  work->paths = pirwa_boxes_paths_copy (paths);
  work->automatic = automatic;
  work->remember = remember;
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, work, work_free);
  g_task_run_in_thread (task, work_run);
}

void
pirwa_boxes_scan_async (const PirwaBoxesPaths *paths, gboolean automatic, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer data)
{
  start_work (paths, automatic, FALSE, cancellable, callback, data);
}
PirwaBoxesScan *
pirwa_boxes_scan_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
void
pirwa_boxes_remember_async (const PirwaBoxesPaths *paths, GAsyncReadyCallback callback,
                            gpointer data)
{
  start_work (paths, FALSE, TRUE, NULL, callback, data);
}
gboolean
pirwa_boxes_remember_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
