/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-config.h"
#include "pirwa-machine.h"
#include "pirwa-usage.h"

#define PIRWA_TYPE_BACKEND (pirwa_backend_get_type ())
G_DECLARE_FINAL_TYPE (PirwaBackend, pirwa_backend, PIRWA, BACKEND, GObject)

typedef enum
{
  PIRWA_REFRESH,
  PIRWA_RECONNECT,
  PIRWA_START,
  PIRWA_SHUTDOWN,
  PIRWA_REBOOT,
  PIRWA_PAUSE,
  PIRWA_RESUME,
  PIRWA_FORCE_STOP,
  PIRWA_DETAILS,
  PIRWA_EDIT,
  PIRWA_CREATE,
  PIRWA_REMOVE,
  PIRWA_CONSOLE,
  PIRWA_GRAPHICS_FD,
  PIRWA_USAGE,
  PIRWA_IMPORT_BOXES
} PirwaOperation;

typedef struct
{
  PirwaOperation operation;
  guint64 request_id; /* Correlates progress; zero is valid for non-UI callers. */
  const char *uuid;
  const char *source_uri;
  PirwaCreate options;
  gboolean eject;
  gboolean delete_disk;
} PirwaRequest;

typedef struct
{
  guint64 serial; /* Monotonic completion order within one backend. */
  GPtrArray *machines;
  GStrv pools;
  PirwaDetails *details;
  char *uuid;
  char *host;
  GSubprocess *viewer;
  gboolean native_console;
  char *console_name;
  GUnixFDList *graphics_fds; /* Owns authenticated sockets, including discarded replies. */
  PirwaUsage usage;
} PirwaReply;

PirwaBackend *pirwa_backend_new (const char *uri);
const char *pirwa_backend_get_uri (PirwaBackend *self);
void pirwa_backend_stop (PirwaBackend *self);
void pirwa_backend_request_async (PirwaBackend *self, const PirwaRequest *request,
                                  GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer user_data);
PirwaReply *pirwa_backend_request_finish (PirwaBackend *self, GAsyncResult *result, GError **error);
void pirwa_reply_free (PirwaReply *reply);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaReply, pirwa_reply_free)

guint64 pirwa_backend_result_serial (GAsyncResult *result);
