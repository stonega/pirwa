/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-machine.h"

struct _PirwaMachine
{
  GObject parent_instance;
  char *uuid;
  char *name;
  char *os_name;
  char *state;
  int state_code;
};

G_DEFINE_FINAL_TYPE (PirwaMachine, pirwa_machine, G_TYPE_OBJECT)

static void
pirwa_machine_finalize (GObject *object)
{
  PirwaMachine *self = PIRWA_MACHINE (object);
  g_free (self->uuid);
  g_free (self->name);
  g_free (self->os_name);
  g_free (self->state);
  G_OBJECT_CLASS (pirwa_machine_parent_class)->finalize (object);
}

static void
pirwa_machine_class_init (PirwaMachineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = pirwa_machine_finalize;
  g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 0);
}

static void
pirwa_machine_init (PirwaMachine *self)
{
  (void)self;
}

PirwaMachine *
pirwa_machine_new (const char *uuid, const char *name, const char *state)
{
  return pirwa_machine_new_full (uuid, name, state, 0);
}

PirwaMachine *
pirwa_machine_new_full (const char *uuid, const char *name, const char *state, int state_code)
{
  g_return_val_if_fail (uuid != NULL && name != NULL && state != NULL, NULL);
  PirwaMachine *self = g_object_new (PIRWA_TYPE_MACHINE, NULL);
  self->uuid = g_strdup (uuid);
  self->name = g_strdup (name);
  self->state = g_strdup (state);
  self->state_code = state_code;
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
pirwa_machine_get_title (PirwaMachine *self)
{
  return self->os_name && *self->os_name ? self->os_name : self->name;
}

void
pirwa_machine_set_os_name (PirwaMachine *self, const char *os_name)
{
  if (g_strcmp0 (self->os_name, os_name) == 0)
    return;
  g_free (self->os_name);
  self->os_name = g_strdup (os_name);
  g_signal_emit_by_name (self, "changed");
}

const char *
pirwa_machine_get_state (PirwaMachine *self)
{
  g_return_val_if_fail (PIRWA_IS_MACHINE (self), NULL);
  return self->state;
}

int
pirwa_machine_get_state_code (PirwaMachine *self)
{
  return self->state_code;
}

void
pirwa_machine_update (PirwaMachine *self, PirwaMachine *snapshot)
{
  g_return_if_fail (g_str_equal (self->uuid, snapshot->uuid));
  if (g_strcmp0 (self->name, snapshot->name) == 0 && self->state_code == snapshot->state_code
      && g_strcmp0 (self->state, snapshot->state) == 0
      && g_strcmp0 (self->os_name, snapshot->os_name) == 0)
    return;
  g_free (self->name);
  g_free (self->state);
  self->name = g_strdup (snapshot->name);
  g_free (self->os_name);
  self->os_name = g_strdup (snapshot->os_name);
  self->state = g_strdup (snapshot->state);
  self->state_code = snapshot->state_code;
  g_signal_emit_by_name (self, "changed");
}
