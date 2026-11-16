/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pirwa-backend.h"
#include <errno.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* stdin is controlled by test-libvirt-daemon.py. No libvirt API or retry timer
 * is wrapped: only retain the worker so this test can join it at shutdown. */
static GThread *worker;
GThread *__real_g_thread_new (const gchar *name, GThreadFunc function, gpointer data);

GThread *
__wrap_g_thread_new (const gchar *name, GThreadFunc function, gpointer data)
{
  GThread *thread = __real_g_thread_new (name, function, data);
  if (g_strcmp0 (name, "pirwa-backend") == 0)
    {
      g_assert_null (worker);
      worker = g_thread_ref (thread);
    }
  return thread;
}

typedef struct
{
  PirwaBackend *backend;
  GMainLoop *loop;
  GThread *main_thread;
  GString *input;
  guint input_source;
  guint next_id;
  guint pending;
} Client;

typedef struct
{
  Client *client;
  guint id;
} Pending;

static void
emit (Client *client, const char *kind, guint id, guint64 serial, PirwaReply *reply, GError *error)
{
  g_assert_true (g_thread_self () == client->main_thread);
  if (error)
    {
      g_autofree char *encoded
          = g_base64_encode ((const guchar *)error->message, strlen (error->message));
      g_print ("{\"event\":\"%s\",\"id\":%u,\"serial\":%" G_GUINT64_FORMAT
               ",\"ok\":false,\"cancelled\":%s,\"message_base64\":\"%s\"}\n",
               kind, id, serial,
               g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "true" : "false",
               encoded);
      return;
    }
  g_assert_nonnull (reply);
  g_assert_nonnull (reply->machines);
  g_print ("{\"event\":\"%s\",\"id\":%u,\"serial\":%" G_GUINT64_FORMAT
           ",\"ok\":true,\"machines\":%u,\"host\":%s,\"pools\":%s,\"domains\":[",
           kind, id, serial, reply->machines->len, reply->host ? "true" : "false",
           reply->pools ? "true" : "false");
  for (guint i = 0; i < reply->machines->len; i++)
    {
      PirwaMachine *machine = g_ptr_array_index (reply->machines, i);
      const char *uuid = pirwa_machine_get_uuid (machine);
      g_assert_true (g_uuid_string_is_valid (uuid));
      g_print ("%s{\"uuid\":\"%s\",\"state\":%d}", i ? "," : "", uuid,
               pirwa_machine_get_state_code (machine));
    }
  g_print ("]}\n");
}

static void
completed (GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *pending = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &error);
  emit (pending->client, "reply", pending->id, pirwa_backend_result_serial (result), reply, error);
  pending->client->pending--;
  g_free (pending);
}

static void
inventory (PirwaBackend *backend, PirwaReply *reply, Client *client)
{
  (void)backend;
  emit (client, "inventory", 0, reply->serial, reply, NULL);
}

static void
backend_error (PirwaBackend *backend, guint64 serial, const char *message, Client *client)
{
  (void)backend;
  g_autoptr (GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, message);
  emit (client, "error", 0, serial, NULL, error);
}

static gboolean
command (Client *client, const char *line)
{
  if (g_str_equal (line, "refresh") || g_str_equal (line, "reconnect"))
    {
      PirwaRequest request
          = { .operation = g_str_equal (line, "refresh") ? PIRWA_REFRESH : PIRWA_RECONNECT };
      Pending *pending = g_new (Pending, 1);
      *pending = (Pending){ client, ++client->next_id };
      client->pending++;
      pirwa_backend_request_async (client->backend, &request, NULL, completed, pending);
    }
  else if (g_str_has_prefix (line, "start ") || g_str_has_prefix (line, "pause ")
           || g_str_has_prefix (line, "resume "))
    {
      g_auto (GStrv) parts = g_strsplit (line, " ", -1);
      g_assert_cmpstr (g_getenv ("PIRWA_RUN_KVM_TESTS"), ==, "1");
      g_assert_cmpuint (g_strv_length (parts), ==, 2);
      g_assert_true (g_uuid_string_is_valid (parts[1]));
      PirwaRequest request = { .operation = g_str_equal (parts[0], "start")   ? PIRWA_START
                                            : g_str_equal (parts[0], "pause") ? PIRWA_PAUSE
                                                                              : PIRWA_RESUME,
                               .uuid = parts[1] };
      Pending *pending = g_new (Pending, 1);
      *pending = (Pending){ client, ++client->next_id };
      client->pending++;
      pirwa_backend_request_async (client->backend, &request, NULL, completed, pending);
    }
  else if (g_str_equal (line, "ping"))
    g_print ("{\"event\":\"pong\"}\n");
  else if (g_str_equal (line, "stop"))
    {
      pirwa_backend_stop (client->backend);
      g_print ("{\"event\":\"stopped\"}\n");
    }
  else if (g_str_equal (line, "quit"))
    {
      g_assert_cmpuint (client->pending, ==, 0);
      pirwa_backend_stop (client->backend);
      g_main_loop_quit (client->loop);
      return FALSE;
    }
  else
    g_error ("Unknown daemon QA command");
  return TRUE;
}

static gboolean
input_ready (gint fd, GIOCondition condition, gpointer data)
{
  (void)condition;
  Client *client = data;
  char buffer[128];
  ssize_t length = read (fd, buffer, sizeof buffer);
  if (length < 0 && errno == EINTR)
    return G_SOURCE_CONTINUE;
  if (length <= 0)
    {
      pirwa_backend_stop (client->backend);
      g_main_loop_quit (client->loop);
      client->input_source = 0;
      return G_SOURCE_REMOVE;
    }
  g_string_append_len (client->input, buffer, length);
  g_assert_cmpuint (client->input->len, <, 4096);
  char *newline;
  while ((newline = strchr (client->input->str, '\n')))
    {
      g_autofree char *line = g_strndup (client->input->str, newline - client->input->str);
      g_string_erase (client->input, 0, newline - client->input->str + 1);
      if (!command (client, line))
        {
          client->input_source = 0;
          return G_SOURCE_REMOVE;
        }
    }
  return G_SOURCE_CONTINUE;
}

int
main (void)
{
  const char *root = g_getenv ("PIRWA_DAEMON_TEST_ROOT");
  g_autofree char *parent = root ? g_path_get_dirname (root) : NULL;
  struct stat info;
  if (g_strcmp0 (g_getenv ("PIRWA_RUN_DAEMON_TESTS"), "1") != 0 || geteuid () == 0 || !root
      || !g_str_has_prefix (root, "/tmp/pirwa-libvirt-") || lstat (root, &info) != 0
      || g_strcmp0 (parent, "/tmp") != 0 || !S_ISDIR (info.st_mode) || info.st_uid != geteuid ()
      || (info.st_mode & 0777) != 0700 || g_strcmp0 (g_getenv ("LIBVIRT_AUTOSTART"), "0") != 0)
    {
      g_printerr (
          "Use PIRWA_RUN_DAEMON_TESTS=1 scripts/test-libvirt-daemon.py as a regular user.\n");
      return 77;
    }
  g_autofree char *runtime = g_build_filename (root, "runtime", NULL);
  if (g_strcmp0 (runtime, g_getenv ("XDG_RUNTIME_DIR")) != 0)
    return 77;
  const char *activation = g_getenv ("PIRWA_DAEMON_TEST_SOCKET_ACTIVATION");
  if (activation)
    {
      if (!g_str_equal (activation, "1"))
        return 77;
      const char *keys[]
          = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME" };
      const char *suffixes[] = { "config", "data", "cache", "state" };
      for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
        {
          g_autofree char *path = g_build_filename (root, suffixes[i], NULL);
          if (g_strcmp0 (g_getenv (keys[i]), path) != 0)
            return 77;
        }
    }
  g_autofree char *socket = g_build_filename (runtime, "libvirt", "virtqemud-sock", NULL);
  g_autofree char *escaped = g_uri_escape_string (socket, "/", FALSE);
  g_autofree char *uri = activation ? g_strdup ("qemu:///session")
                                    : g_strconcat ("qemu+unix:///session?socket=", escaped, NULL);
  setvbuf (stdout, NULL, _IOLBF, 0);
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  g_autoptr (PirwaBackend) backend = pirwa_backend_new (uri);
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Client client = {
    .backend = backend, .loop = loop, .main_thread = g_thread_self (), .input = g_string_new (NULL)
  };
  g_signal_connect (backend, "inventory", G_CALLBACK (inventory), &client);
  g_signal_connect (backend, "error", G_CALLBACK (backend_error), &client);
  client.input_source
      = g_unix_fd_add (STDIN_FILENO, G_IO_IN | G_IO_HUP | G_IO_ERR, input_ready, &client);
  g_print ("{\"event\":\"ready\"}\n");
  g_main_loop_run (loop);
  pirwa_backend_stop (backend);
  g_thread_join (worker);
  if (client.input_source)
    g_source_remove (client.input_source);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpuint (client.pending, ==, 0);
  g_string_free (client.input, TRUE);
  g_print ("{\"event\":\"joined\"}\n");
  return 0;
}
