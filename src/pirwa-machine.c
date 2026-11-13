/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-machine.h"

struct _PirwaMachine
{
  GObject parent_instance;
  char *uuid;
  char *name;
  char *state;
};

G_DEFINE_FINAL_TYPE (PirwaMachine, pirwa_machine, G_TYPE_OBJECT)

static void
pirwa_machine_finalize (GObject *object)
{
  PirwaMachine *self = PIRWA_MACHINE (object);
  g_free (self->uuid);
  g_free (self->name);
  g_free (self->state);
  G_OBJECT_CLASS (pirwa_machine_parent_class)->finalize (object);
}

static void
pirwa_machine_class_init (PirwaMachineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = pirwa_machine_finalize;
}

static void
pirwa_machine_init (PirwaMachine *self)
{
  (void)self;
}

PirwaMachine *
pirwa_machine_new (const char *uuid, const char *name, const char *state)
{
  g_return_val_if_fail (uuid != NULL && name != NULL && state != NULL, NULL);
  PirwaMachine *self = g_object_new (PIRWA_TYPE_MACHINE, NULL);
  self->uuid = g_strdup (uuid);
  self->name = g_strdup (name);
  self->state = g_strdup (state);
  return self;
}

const char *
pirwa_machine_get_uuid (PirwaMachine *self)
{
  g_return_val_if_fail (PIRWA_IS_MACHINE (self), NULL);
  return self->uuid;
}

const char *
pirwa_machine_get_name (PirwaMachine *self)
{
  g_return_val_if_fail (PIRWA_IS_MACHINE (self), NULL);
  return self->name;
}

const char *
pirwa_machine_get_state (PirwaMachine *self)
{
  g_return_val_if_fail (PIRWA_IS_MACHINE (self), NULL);
  return self->state;
}
