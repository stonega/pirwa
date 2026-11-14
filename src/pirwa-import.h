/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-config.h"

typedef struct
{
  char *xml;
  char *disk;
  char *loader;
  char *nvram;
  char *uuid;
} PirwaImport;

/* Only prepares a copy; never rewrites the source definition. */
PirwaImport *pirwa_import_prepare (const char *xml, GError **error);
char *pirwa_import_domain (const PirwaImport *plan, const char *disk, const char *loader,
                           const char *nvram, const char *pool, const char *key);
void pirwa_import_free (PirwaImport *plan);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PirwaImport, pirwa_import_free)

/* Copies a bounded regular firmware file into a new private file, with cancellation. */
gboolean pirwa_import_copy_firmware (const char *source, const char *target,
                                     GCancellable *cancellable, GError **error);
