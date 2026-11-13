/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pirwa-boxes.h"
#include <adwaita.h>

typedef enum
{
  PIRWA_BOXES_SKIP,
  PIRWA_BOXES_USE,
  PIRWA_BOXES_RESCAN
} PirwaBoxesResponse;
typedef void (*PirwaBoxesResponseFunc) (GtkWindow *parent, PirwaBoxesResponse response,
                                        const char *uri, const char *uuid, gboolean flatpak);
/* Takes ownership of scan. */
void pirwa_boxes_dialog_present (GtkWindow *parent, PirwaBoxesScan *scan,
                                 PirwaBoxesResponseFunc response);
