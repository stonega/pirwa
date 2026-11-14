/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-machine.h"
#include <gio/gio.h>

typedef struct
{
  char *native_uri;
  char *state_file;
  char *flatpak_root;
  char *runtime_root;
  char *flatpak_program;
} PirwaBoxesPaths;

typedef struct
{
  char *uri;
  gboolean available;
  GPtrArray *machines;
} PirwaBoxesLibrary;

typedef struct
{
  GPtrArray *libraries;
  gboolean already_seen;
  char *warning;
  gboolean installed;
} PirwaBoxesScan;

PirwaBoxesPaths *pirwa_boxes_paths_new (const char *native_uri);
PirwaBoxesPaths *pirwa_boxes_paths_copy (const PirwaBoxesPaths *paths);
void pirwa_boxes_paths_free (PirwaBoxesPaths *paths);
void pirwa_boxes_scan_free (PirwaBoxesScan *scan);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaBoxesPaths, pirwa_boxes_paths_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaBoxesScan, pirwa_boxes_scan_free)

/* All synchronous discovery/state functions run on workers in the application. */
PirwaBoxesScan *pirwa_boxes_scan (const PirwaBoxesPaths *paths, gboolean automatic,
                                  GCancellable *cancellable, GError **error);
gboolean pirwa_boxes_remember (const PirwaBoxesPaths *paths, GCancellable *cancellable,
                               GError **error);
void pirwa_boxes_scan_async (const PirwaBoxesPaths *paths, gboolean automatic,
                             GCancellable *cancellable, GAsyncReadyCallback callback,
                             gpointer data);
PirwaBoxesScan *pirwa_boxes_scan_finish (GAsyncResult *result, GError **error);
void pirwa_boxes_remember_async (const PirwaBoxesPaths *paths, GAsyncReadyCallback callback,
                                 gpointer data);
gboolean pirwa_boxes_remember_finish (GAsyncResult *result, GError **error);
