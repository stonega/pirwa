/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit disposable-guest tooling. Never registered with meson test. */
#include "pirwa-backend.h"
#include "pirwa-provision.h"
#include <libvirt/libvirt.h>

typedef struct
{
  GMainLoop *loop;
  PirwaReply *reply;
  GError *error;
} Result;

static void
completed (GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  r->reply = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &r->error);
  g_main_loop_quit (r->loop);
}

static PirwaReply *
request (PirwaBackend *backend, const PirwaRequest *operation, GError **error)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  Result r = { .loop = loop };
  pirwa_backend_request_async (backend, operation, NULL, completed, &r);
  g_main_loop_run (loop);
  if (r.error)
    g_propagate_error (error, r.error);
  return r.reply;
}

static gboolean
fixture_name (const char *name)
{
  return name && g_str_has_prefix (name, "pirwa-qa-") && g_uuid_string_is_valid (name + 9);
}

static gboolean
fixture_domain (virConnectPtr connection, virDomainPtr domain, const char *uuid)
{
  const char *name = virDomainGetName (domain);
  if (fixture_name (name))
    return TRUE;
  g_autofree char *expected = g_strdup_printf ("pirwa-%s", uuid);
  if (g_strcmp0 (name, expected) != 0)
    return FALSE;
  g_autofree char *title = virDomainGetMetadata (domain, VIR_DOMAIN_METADATA_TITLE, NULL, 0);
  if (!fixture_name (title))
    return FALSE;
  g_autofree char *xml = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  g_autoptr (PirwaDetails) details = xml ? pirwa_config_parse (xml, NULL) : NULL;
  return details && pirwa_provision_owned (connection, uuid, details);
}

static gboolean
benchmark (PirwaBackend *backend, virDomainPtr domain, const char *uuid, guint count,
           GError **error)
{
  PirwaRequest refresh = { .operation = PIRWA_REFRESH };
  g_autoptr (PirwaReply) ready = request (backend, &refresh, error);
  if (!ready)
    return FALSE;
  /* Alternate order. Each sample starts the same stopped definition. Guest boot
   * and a graphical connection are deliberately outside this timing span. */
  for (guint i = 0; i < count; i++)
    {
      double times[2];
      for (guint j = 0; j < 2; j++)
        {
          guint method = (i + j) % 2;
          if (virDomainIsActive (domain) != 0)
            return FALSE;
          gint64 start = g_get_monotonic_time ();
          if (method == 0)
            {
              if (virDomainCreate (domain) < 0)
                return FALSE;
            }
          else
            {
              PirwaRequest op = { .operation = PIRWA_START, .uuid = uuid };
              g_autoptr (PirwaReply) reply = request (backend, &op, error);
              if (!reply)
                return FALSE;
            }
          times[method] = (g_get_monotonic_time () - start) / 1000.0;
          if (virDomainIsActive (domain) != 1 || virDomainDestroy (domain) < 0)
            return FALSE;
        }
      g_print ("{\"pair\":%u,\"first\":\"%s\",\"direct_ms\":%.3f,\"pirwa_ms\":%.3f}\n", i,
               i % 2 ? "pirwa" : "direct", times[0], times[1]);
    }
  return TRUE;
}

int
main (int argc, char **argv)
{
  if (g_strcmp0 (g_getenv ("PIRWA_RUN_KVM_TESTS"), "1") != 0)
    {
      g_printerr ("Set PIRWA_RUN_KVM_TESTS=1 for disposable-guest QA.\n");
      return 77;
    }
  if (argc < 4)
    {
      g_printerr ("Usage: qa-vm URI create|import NAME SOURCE POOL [MEMORY_MB]\n"
                  "       qa-vm URI start|stop|remove|console UUID\n"
                  "       qa-vm URI benchmark-start UUID PAIRS\n"
                  "Names must be pirwa-qa- followed by a UUID. Resources are not auto-removed.\n");
      return 2;
    }
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaBackend) backend = NULL;
  virConnectPtr connection = virConnectOpen (argv[1]);
  virDomainPtr domain = NULL;
  gboolean ok = FALSE;
  if (!connection)
    goto out;
  gboolean create = g_str_equal (argv[2], "create") || g_str_equal (argv[2], "import");
  guint64 memory_mb = 4096;
  if (create ? (argc != 6 && argc != 7) || !fixture_name (argv[3])
             : !g_uuid_string_is_valid (argv[3]))
    goto out;
  if (create && argc == 7
      && !g_ascii_string_to_unsigned (argv[6], 10, 2048, 8192, &memory_mb, &error))
    goto out;
  if (!create)
    {
      domain = virDomainLookupByUUIDString (connection, argv[3]);
      if (!domain || !fixture_domain (connection, domain, argv[3]))
        goto out;
    }
  backend = pirwa_backend_new (argv[1]);
  if (g_str_equal (argv[2], "benchmark-start"))
    {
      guint64 count;
      if (argc == 5 && g_ascii_string_to_unsigned (argv[4], 10, 1, 100, &count, &error))
        ok = benchmark (backend, domain, argv[3], (guint)count, &error);
      goto out;
    }
  const char *names[] = { "start", "stop", "remove", "console", "create", "import" };
  PirwaOperation operations[]
      = { PIRWA_START, PIRWA_FORCE_STOP, PIRWA_REMOVE, PIRWA_CONSOLE, PIRWA_CREATE, PIRWA_CREATE };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_str_equal (argv[2], names[i]))
      {
        PirwaRequest op
            = { .operation = operations[i], .uuid = create ? NULL : argv[3], .delete_disk = TRUE };
        if (create)
          op.options = (PirwaCreate){ .name = argv[3],
                                      .source = argv[4],
                                      .pool = argv[5],
                                      .cpus = 4,
                                      .memory_mb = (guint)memory_mb,
                                      .disk_gb = 16,
                                      .import_image = i == 5 };
        g_autoptr (PirwaReply) reply = request (backend, &op, &error);
        ok = reply != NULL;
        if (ok)
          g_print ("{\"uuid\":\"%s\"}\n", reply->uuid ? reply->uuid : argv[3]);
        break;
      }
out:
  if (!ok)
    g_printerr ("QA command failed: %s\n", error ? error->message : "identity/libvirt check");
  if (backend)
    pirwa_backend_stop (backend);
  if (domain)
    virDomainFree (domain);
  if (connection)
    virConnectClose (connection);
  return ok ? 0 : 1;
}
