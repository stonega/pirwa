/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-boxes.h"
#include <adwaita.h>

typedef enum
{
  PIRWA_BOXES_SKIP,
  PIRWA_BOXES_IMPORT,
  PIRWA_BOXES_RESCAN,
  PIRWA_BOXES_CANCEL
} PirwaBoxesResponse;
typedef void (*PirwaBoxesResponseFunc) (GtkWindow *parent, AdwDialog *dialog,
                                        PirwaBoxesResponse response, const char *uri,
                                        const char *uuid);
/* Takes ownership of scan. */
void pirwa_boxes_dialog_present (GtkWindow *parent, PirwaBoxesScan *scan,
                                 PirwaBoxesResponseFunc response);
