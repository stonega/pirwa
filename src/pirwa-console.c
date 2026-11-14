/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-console.h"
#include "config.h"
#include "pirwa-backend.h"
#include "pirwa-frame.h"
#include <glib/gi18n.h>
#include <math.h>
#include <unistd.h>

typedef struct
{
  GtkWidget parent_instance;
  PirwaConsole *console; /* Owned by the containing window. */
} PirwaDisplay;
typedef GtkWidgetClass PirwaDisplayClass;
G_DEFINE_TYPE (PirwaDisplay, pirwa_display, GTK_TYPE_WIDGET)

struct _PirwaConsole
{
  AdwWindow parent_instance;
  PirwaBackend *backend;
  GCancellable *pending;
  SpiceSession *session;
  SpiceMainChannel *main;
  SpiceInputsChannel *inputs;
  SpiceChannel *display_channel;
  SpiceDisplayPrimary primary;
  GdkTexture *texture;
  cairo_region_t *damage;
  GtkWidget *display;
  GtkWidget *release;
  GtkStack *stack;
  AdwStatusPage *status;
  GtkLabel *hint;
  char *uuid;
  guint generation;
  guint reconnect_source;
  guint resize_source;
  guint retries;
  guint buttons;
  gboolean keys[512];
  gboolean input_ready;
  gboolean captured;
  gboolean closed;
  gboolean running;
  gboolean have_pointer;
  double pointer_x;
  double pointer_y;
  double scroll_y;
};
G_DEFINE_FINAL_TYPE (PirwaConsole, pirwa_console, ADW_TYPE_WINDOW)

static void start_session (PirwaConsole *self);
static void disconnect_session (PirwaConsole *self);

static void
show_status (PirwaConsole *self, const char *title, const char *description)
{
  adw_status_page_set_title (self->status, title);
  g_autofree char *escaped = g_markup_escape_text (description, -1);
  adw_status_page_set_description (self->status, escaped);
  gtk_stack_set_visible_child_name (self->stack, "status");
}

static GdkToplevel *
toplevel (PirwaConsole *self)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (self));
  return surface && GDK_IS_TOPLEVEL (surface) ? GDK_TOPLEVEL (surface) : NULL;
}

static void
release_input (PirwaConsole *self)
{
  if (self->inputs && self->input_ready)
    {
      for (guint i = 1; i < G_N_ELEMENTS (self->keys); i++)
        if (self->keys[i])
          spice_inputs_channel_key_release (self->inputs, i);
      for (guint i = 1; i <= 7; i++)
        if (self->buttons & (1u << (i - 1)))
          {
            self->buttons &= ~(1u << (i - 1));
            spice_inputs_channel_button_release (self->inputs, i, self->buttons);
          }
    }
  memset (self->keys, 0, sizeof self->keys);
  self->buttons = 0;
  self->captured = FALSE;
  self->have_pointer = FALSE;
  self->scroll_y = 0;
  GdkToplevel *surface = toplevel (self);
  if (surface)
    gdk_toplevel_restore_system_shortcuts (surface);
  gtk_label_set_text (self->hint, _ ("Click the display to send input to the machine"));
}

static void
release_clicked (GtkButton *button, PirwaConsole *self)
{
  (void)button;
  release_input (self);
  gtk_widget_grab_focus (self->release);
}

static gboolean
send_resize (gpointer data)
{
  PirwaConsole *self = data;
  self->resize_source = 0;
  gboolean connected = FALSE;
  if (self->main)
    g_object_get (self->main, "agent-connected", &connected, NULL);
  int width = gtk_widget_get_width (self->display);
  int height = gtk_widget_get_height (self->display);
  if (!self->closed && connected && width >= 320 && height >= 200)
    {
      spice_main_channel_update_display_enabled (self->main, 0, TRUE, FALSE);
      spice_main_channel_update_display (self->main, 0, 0, 0, MIN (width, 8192), MIN (height, 8192),
                                         TRUE);
    }
  return G_SOURCE_REMOVE;
}

static void
schedule_resize (PirwaConsole *self)
{
  if (self->resize_source)
    g_source_remove (self->resize_source);
  self->resize_source = g_timeout_add (200, send_resize, self);
}

static void
pirwa_display_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  (void)width;
  (void)height;
  (void)baseline;
  PirwaConsole *self = ((PirwaDisplay *)widget)->console;
  if (self && !self->closed)
    schedule_resize (self);
}

static void
pirwa_display_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  PirwaConsole *self = ((PirwaDisplay *)widget)->console;
  if (!self || self->closed)
    return;
  graphene_rect_t bounds
      = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  const GdkRGBA black = { 0, 0, 0, 1 };
  gtk_snapshot_append_color (snapshot, &black, &bounds);
  if (self->primary.data && self->damage && !cairo_region_is_empty (self->damage))
    {
      GdkTexture *frame = pirwa_frame_texture (&self->primary, self->texture, self->damage);
      if (frame)
        g_set_object (&self->texture, frame);
      g_clear_object (&frame);
      g_clear_pointer (&self->damage, cairo_region_destroy);
    }
  if (!self->texture)
    return;
  int width = gdk_texture_get_width (self->texture);
  int height = gdk_texture_get_height (self->texture);
  double scale = MIN (bounds.size.width / width, bounds.size.height / height);
  graphene_rect_t rect = GRAPHENE_RECT_INIT ((bounds.size.width - width * scale) / 2,
                                             (bounds.size.height - height * scale) / 2,
                                             width * scale, height * scale);
  gtk_snapshot_append_texture (snapshot, self->texture, &rect);
}

static void
pirwa_display_class_init (PirwaDisplayClass *klass)
{
  klass->snapshot = pirwa_display_snapshot;
  klass->size_allocate = pirwa_display_size_allocate;
  gtk_widget_class_set_accessible_role (klass, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
pirwa_display_init (PirwaDisplay *self)
{
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Virtual machine display"), -1);
}

static void
invalidate (SpiceChannel *channel, int x, int y, int width, int height, PirwaConsole *self)
{
  if (channel != self->display_channel || !self->primary.data || width <= 0 || height <= 0)
    return;
  cairo_rectangle_int_t rect = { MAX (0, x), MAX (0, y), 0, 0 };
  rect.width = MIN ((gint64)self->primary.width, (gint64)x + width) - rect.x;
  rect.height = MIN ((gint64)self->primary.height, (gint64)y + height) - rect.y;
  if (rect.width <= 0 || rect.height <= 0)
    return;
  if (!self->damage)
    self->damage = cairo_region_create ();
  cairo_region_union_rectangle (self->damage, &rect);
  /* GTK coalesces invalidations. No continuous tick, screenshot timer, or redraw
   * runs while the guest is idle or this window is hidden. */
  gtk_widget_queue_draw (self->display);
}

static void
primary_destroy (SpiceChannel *channel, PirwaConsole *self)
{
  if (channel != self->display_channel)
    return;
  memset (&self->primary, 0, sizeof self->primary);
  g_clear_object (&self->texture);
  g_clear_pointer (&self->damage, cairo_region_destroy);
  gtk_widget_queue_draw (self->display);
}

static void
primary_create (SpiceChannel *channel, int format, int width, int height, int stride, int shmid,
                gpointer data, PirwaConsole *self)
{
  primary_destroy (channel, self);
  self->primary = (SpiceDisplayPrimary){ .format = format,
                                         .width = width,
                                         .height = height,
                                         .stride = stride,
                                         .shmid = shmid,
                                         .data = data };
  /* Validate before exposing the display; malformed/unimplemented formats are
   * reported rather than leaving a permanently blank window. */
  self->texture = pirwa_frame_texture (&self->primary, NULL, NULL);
  if (!self->texture)
    {
      show_status (self, _ ("Unsupported display"),
                   _ ("This console supports RGB displays up to 8192 × 8192 pixels."));
      return;
    }
  self->retries = 0;
  gtk_stack_set_visible_child_name (self->stack, "display");
  invalidate (channel, 0, 0, width, height, self);
  schedule_resize (self);
}

static void
cursor_set (SpiceChannel *channel, int width, int height, int hot_x, int hot_y, gpointer rgba,
            PirwaConsole *self)
{
  (void)channel;
  if (!rgba || width < 1 || height < 1 || width > 512 || height > 512)
    return;
  g_autoptr (GBytes) bytes = g_bytes_new (rgba, (gsize)width * height * 4);
  g_autoptr (GdkTexture) texture
      = gdk_memory_texture_new (width, height, GDK_MEMORY_R8G8B8A8, bytes, (gsize)width * 4);
  g_autoptr (GdkCursor) cursor = gdk_cursor_new_from_texture (texture, CLAMP (hot_x, 0, width - 1),
                                                              CLAMP (hot_y, 0, height - 1), NULL);
  gtk_widget_set_cursor (self->display, cursor);
}

static void
cursor_hide (SpiceChannel *channel, PirwaConsole *self)
{
  (void)channel;
  gtk_widget_set_cursor_from_name (self->display, "none");
}

static void
cursor_reset (SpiceChannel *channel, PirwaConsole *self)
{
  (void)channel;
  gtk_widget_set_cursor (self->display, NULL);
}

static gboolean
retry_session (gpointer data)
{
  PirwaConsole *self = data;
  self->reconnect_source = 0;
  disconnect_session (self);
  if (!self->closed && self->running && self->retries++ < 3)
    start_session (self);
  return G_SOURCE_REMOVE;
}

static void
channel_event (SpiceChannel *channel, SpiceChannelEvent event, PirwaConsole *self)
{
  if (channel == SPICE_CHANNEL (self->inputs))
    self->input_ready = event == SPICE_CHANNEL_OPENED;
  if (event == SPICE_CHANNEL_OPENED || event == SPICE_CHANNEL_SWITCHING)
    {
      if (event == SPICE_CHANNEL_OPENED && SPICE_IS_MAIN_CHANNEL (channel))
        spice_main_channel_request_mouse_mode (SPICE_MAIN_CHANNEL (channel),
                                               SPICE_MOUSE_MODE_CLIENT);
      return;
    }
  if ((SPICE_IS_MAIN_CHANNEL (channel) || channel == self->display_channel) && !self->closed)
    {
      release_input (self);
      const GError *error = spice_channel_get_error (channel);
      show_status (self, _ ("Display disconnected"),
                   error ? error->message : _ ("Waiting for the machine. You can also reconnect."));
      /* Disconnect outside the SPICE signal/coroutine. Lifecycle events handle
       * stopped guests; transient disconnects get at most three retries. */
      if (!self->reconnect_source)
        self->reconnect_source = g_timeout_add (1000, retry_session, self);
    }
}

typedef struct
{
  GWeakRef window;
  SpiceChannel *channel;
  guint generation;
} SocketRequest;

static void
socket_done (GObject *source, GAsyncResult *result, gpointer data)
{
  SocketRequest *request = data;
  g_autoptr (PirwaConsole) self = g_weak_ref_get (&request->window);
  g_autoptr (GError) error = NULL;
  g_autoptr (PirwaReply) reply
      = pirwa_backend_request_finish (PIRWA_BACKEND (source), result, &error);
  if (self && !self->closed && self->generation == request->generation)
    {
      if (reply && reply->graphics_fds)
        {
          int fd = g_unix_fd_list_get (reply->graphics_fds, 0, &error);
          /* The channel takes ownership of a valid fd even when connect fails. */
          if (fd >= 0 && !spice_channel_open_fd (request->channel, fd))
            spice_channel_disconnect (request->channel, SPICE_CHANNEL_ERROR_CONNECT);
        }
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          show_status (self, _ ("Cannot open display"), error->message);
          spice_channel_disconnect (request->channel, SPICE_CHANNEL_NONE);
        }
    }
  g_weak_ref_clear (&request->window);
  g_object_unref (request->channel);
  g_free (request);
}

static void
open_socket (SpiceChannel *channel, int with_tls, PirwaConsole *self)
{
  (void)with_tls; /* libvirt supplies an authenticated local socket/tunnel. */
  if (self->closed)
    return;
  SocketRequest *context = g_new0 (SocketRequest, 1);
  g_weak_ref_init (&context->window, self);
  context->channel = g_object_ref (channel);
  context->generation = self->generation;
  PirwaRequest request = { .operation = PIRWA_GRAPHICS_FD, .uuid = self->uuid };
  pirwa_backend_request_async (self->backend, &request, self->pending, socket_done, context);
}

static void
agent_update (SpiceChannel *channel, PirwaConsole *self)
{
  (void)channel;
  schedule_resize (self);
}

static void
channel_new (SpiceSession *session, SpiceChannel *channel, PirwaConsole *self)
{
  int type, id;
  g_object_get (channel, "channel-type", &type, "channel-id", &id, NULL);
  /* Playback only. Do not connect microphone, USB, smartcard, or file channels.
   * Stop the audio helper's after-handler from opening the record channel. */
  if (type == SPICE_CHANNEL_RECORD)
    {
      g_signal_stop_emission_by_name (session, "channel-new");
      return;
    }
  if (type != SPICE_CHANNEL_MAIN && type != SPICE_CHANNEL_DISPLAY && type != SPICE_CHANNEL_INPUTS
      && type != SPICE_CHANNEL_CURSOR && type != SPICE_CHANNEL_PLAYBACK)
    return;
  if ((type == SPICE_CHANNEL_DISPLAY || type == SPICE_CHANNEL_CURSOR) && id != 0)
    return;
  g_signal_connect_object (channel, "open-fd", G_CALLBACK (open_socket), self, 0);
  g_signal_connect_object (channel, "channel-event", G_CALLBACK (channel_event), self, 0);
  if (SPICE_IS_MAIN_CHANNEL (channel))
    {
      self->main = SPICE_MAIN_CHANNEL (channel);
      g_signal_connect_object (channel, "main-agent-update", G_CALLBACK (agent_update), self, 0);
      return; /* spice_session_open_fd connects the main channel. */
    }
  if (SPICE_IS_DISPLAY_CHANNEL (channel))
    {
      self->display_channel = channel;
      g_signal_connect_object (channel, "display-primary-create", G_CALLBACK (primary_create), self,
                               0);
      g_signal_connect_object (channel, "display-primary-destroy", G_CALLBACK (primary_destroy),
                               self, 0);
      g_signal_connect_object (channel, "display-invalidate", G_CALLBACK (invalidate), self, 0);
    }
  else if (SPICE_IS_INPUTS_CHANNEL (channel))
    self->inputs = SPICE_INPUTS_CHANNEL (channel);
  else if (SPICE_IS_CURSOR_CHANNEL (channel))
    {
      g_signal_connect_object (channel, "cursor-set", G_CALLBACK (cursor_set), self, 0);
      g_signal_connect_object (channel, "cursor-hide", G_CALLBACK (cursor_hide), self, 0);
      g_signal_connect_object (channel, "cursor-reset", G_CALLBACK (cursor_reset), self, 0);
    }
  if (type != SPICE_CHANNEL_PLAYBACK) /* The audio helper connects playback. */
    spice_channel_connect (channel);
}

static void
channel_destroy (SpiceSession *session, SpiceChannel *channel, PirwaConsole *self)
{
  (void)session;
  if (channel == self->display_channel)
    {
      primary_destroy (channel, self);
      self->display_channel = NULL;
    }
  if (channel == SPICE_CHANNEL (self->inputs))
    {
      release_input (self);
      self->inputs = NULL;
      self->input_ready = FALSE;
    }
  if (channel == SPICE_CHANNEL (self->main))
    self->main = NULL;
}

static void
disconnect_session (PirwaConsole *self)
{
  release_input (self);
  self->generation++;
  if (self->pending)
    g_cancellable_cancel (self->pending);
  g_clear_object (&self->pending);
  if (self->display_channel)
    primary_destroy (self->display_channel, self);
  self->display_channel = NULL;
  self->main = NULL;
  self->inputs = NULL;
  self->input_ready = FALSE;
  if (self->session)
    {
      g_autoptr (GList) channels = spice_session_get_channels (self->session);
      for (GList *item = channels; item; item = item->next)
        g_signal_handlers_disconnect_by_data (item->data, self);
      g_signal_handlers_disconnect_by_data (self->session, self);
      spice_session_disconnect (self->session);
      g_clear_object (&self->session);
    }
  gtk_widget_set_cursor (self->display, NULL);
}

static void
start_session (PirwaConsole *self)
{
  if (self->closed || self->session)
    return;
  self->pending = g_cancellable_new ();
  self->session = spice_session_new ();
  g_object_set (self->session, "enable-usbredir", FALSE, "enable-smartcard", FALSE, NULL);
  g_signal_connect_object (self->session, "channel-new", G_CALLBACK (channel_new), self, 0);
  g_signal_connect_object (self->session, "channel-destroy", G_CALLBACK (channel_destroy), self, 0);
  spice_audio_get (self->session, NULL);
  show_status (self, _ ("Connecting to display"), _ ("Waiting for the machine’s first frame…"));
  if (!spice_session_open_fd (self->session, -1))
    show_status (self, _ ("Cannot open display"), _ ("Choose Reconnect to try again."));
}

static void
inventory_changed (PirwaBackend *backend, PirwaReply *reply, PirwaConsole *self)
{
  (void)backend;
  gboolean running = FALSE;
  for (guint i = 0; reply->machines && i < reply->machines->len; i++)
    {
      PirwaMachine *machine = g_ptr_array_index (reply->machines, i);
      if (g_str_equal (pirwa_machine_get_uuid (machine), self->uuid))
        {
          int state = pirwa_machine_get_state_code (machine);
          running = state == 1 || state == 3;
          break;
        }
    }
  self->running = running;
  if (!running)
    {
      if (self->reconnect_source)
        g_clear_handle_id (&self->reconnect_source, g_source_remove);
      disconnect_session (self);
      show_status (self, _ ("Machine is stopped"),
                   _ ("The display will reconnect when it starts."));
    }
  else if (!self->session)
    {
      self->retries = 0;
      start_session (self);
    }
}

static void
reconnect_clicked (GtkButton *button, PirwaConsole *self)
{
  (void)button;
  g_clear_handle_id (&self->reconnect_source, g_source_remove);
  disconnect_session (self);
  self->retries = 0;
  start_session (self);
}

static gboolean
key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
             PirwaConsole *self)
{
  (void)controller;
  (void)keyval;
  (void)state;
  if (!self->captured || !self->inputs || !self->input_ready)
    return FALSE;
  guint scan = pirwa_frame_scancode (keycode);
  if (!scan || scan >= G_N_ELEMENTS (self->keys))
    return TRUE;
  self->keys[scan] = TRUE;
  if ((self->keys[0x1d] || self->keys[0x11d]) && (self->keys[0x38] || self->keys[0x138]))
    {
      release_clicked (NULL, self);
      return TRUE;
    }
  spice_inputs_channel_key_press (self->inputs, scan);
  return TRUE;
}

static void
key_released (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
              PirwaConsole *self)
{
  (void)controller;
  (void)keyval;
  (void)state;
  guint scan = pirwa_frame_scancode (keycode);
  if (scan < G_N_ELEMENTS (self->keys) && self->keys[scan])
    {
      if (self->inputs && self->input_ready)
        spice_inputs_channel_key_release (self->inputs, scan);
      self->keys[scan] = FALSE;
    }
}

static gboolean
pointer_position (PirwaConsole *self, double x, double y)
{
  if (!self->captured || !self->inputs || !self->input_ready)
    return FALSE;
  int guest_x, guest_y;
  if (!pirwa_frame_position (gtk_widget_get_width (self->display),
                             gtk_widget_get_height (self->display), self->primary.width,
                             self->primary.height, x, y, &guest_x, &guest_y))
    return FALSE;
  int mode = SPICE_MOUSE_MODE_CLIENT;
  if (self->main)
    g_object_get (self->main, "mouse-mode", &mode, NULL);
  if (mode == SPICE_MOUSE_MODE_CLIENT)
    spice_inputs_channel_position (self->inputs, guest_x, guest_y, 0, self->buttons);
  else if (self->have_pointer)
    spice_inputs_channel_motion (self->inputs, (int)(x - self->pointer_x),
                                 (int)(y - self->pointer_y), self->buttons);
  self->pointer_x = x;
  self->pointer_y = y;
  self->have_pointer = TRUE;
  return TRUE;
}

static void
pointer_motion (GtkEventControllerMotion *controller, double x, double y, PirwaConsole *self)
{
  (void)controller;
  pointer_position (self, x, y);
}

static guint
mouse_button (guint button)
{
  switch (button)
    {
    case 1:
      return SPICE_MOUSE_BUTTON_LEFT;
    case 2:
      return SPICE_MOUSE_BUTTON_MIDDLE;
    case 3:
      return SPICE_MOUSE_BUTTON_RIGHT;
    case 8:
      return SPICE_MOUSE_BUTTON_SIDE;
    case 9:
      return SPICE_MOUSE_BUTTON_EXTRA;
    default:
      return 0;
    }
}

static void
pointer_pressed (GtkGestureClick *gesture, int count, double x, double y, PirwaConsole *self)
{
  (void)count;
  if (!self->inputs || !self->input_ready)
    return;
  self->captured = TRUE;
  gtk_widget_grab_focus (self->display);
  gtk_label_set_text (self->hint, _ ("Press Ctrl+Alt to release input"));
  GdkToplevel *surface = toplevel (self);
  if (surface)
    gdk_toplevel_inhibit_system_shortcuts (
        surface, gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (gesture)));
  guint button
      = mouse_button (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)));
  if (button && pointer_position (self, x, y))
    {
      self->buttons |= 1u << (button - 1);
      spice_inputs_channel_button_press (self->inputs, button, self->buttons);
    }
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
pointer_released (GtkGestureClick *gesture, int count, double x, double y, PirwaConsole *self)
{
  (void)count;
  (void)x;
  (void)y;
  guint button
      = mouse_button (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)));
  if (button && (self->buttons & (1u << (button - 1))))
    {
      self->buttons &= ~(1u << (button - 1));
      if (self->inputs && self->input_ready)
        spice_inputs_channel_button_release (self->inputs, button, self->buttons);
    }
}

static gboolean
pointer_scroll (GtkEventControllerScroll *controller, double dx, double dy, PirwaConsole *self)
{
  (void)controller;
  (void)dx;
  if (!self->captured || !self->inputs || !self->input_ready)
    return FALSE;
  self->scroll_y = CLAMP (self->scroll_y + dy, -32, 32);
  while (fabs (self->scroll_y) >= 1)
    {
      guint button = self->scroll_y > 0 ? SPICE_MOUSE_BUTTON_DOWN : SPICE_MOUSE_BUTTON_UP;
      spice_inputs_channel_button_press (self->inputs, button, self->buttons);
      spice_inputs_channel_button_release (self->inputs, button, self->buttons);
      self->scroll_y += self->scroll_y > 0 ? -1 : 1;
    }
  return TRUE;
}

static void
focus_left (GtkEventControllerFocus *controller, PirwaConsole *self)
{
  (void)controller;
  release_input (self);
}

static void
active_changed (PirwaConsole *self, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  if (!gtk_window_is_active (GTK_WINDOW (self)))
    release_input (self);
}

static void
fullscreen_clicked (GtkButton *button, PirwaConsole *self)
{
  (void)button;
  if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
    gtk_window_unfullscreen (GTK_WINDOW (self));
  else
    gtk_window_fullscreen (GTK_WINDOW (self));
}

static void
stop_console (PirwaConsole *self)
{
  if (self->closed)
    return;
  self->closed = TRUE;
  g_clear_handle_id (&self->reconnect_source, g_source_remove);
  g_clear_handle_id (&self->resize_source, g_source_remove);
  disconnect_session (self);
  if (self->backend)
    pirwa_backend_stop (self->backend);
}

static gboolean
close_requested (GtkWindow *window, gpointer data)
{
  (void)data;
  stop_console (PIRWA_CONSOLE_WINDOW (window));
  return FALSE;
}

static void
pirwa_console_dispose (GObject *object)
{
  PirwaConsole *self = PIRWA_CONSOLE_WINDOW (object);
  stop_console (self);
  g_clear_object (&self->backend);
  g_clear_object (&self->texture);
  g_clear_pointer (&self->damage, cairo_region_destroy);
  G_OBJECT_CLASS (pirwa_console_parent_class)->dispose (object);
}

static void
pirwa_console_finalize (GObject *object)
{
  g_free (PIRWA_CONSOLE_WINDOW (object)->uuid);
  G_OBJECT_CLASS (pirwa_console_parent_class)->finalize (object);
}

static void
pirwa_console_class_init (PirwaConsoleClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_console_dispose;
  G_OBJECT_CLASS (klass)->finalize = pirwa_console_finalize;
}

static void
pirwa_console_init (PirwaConsole *self)
{
  gtk_window_set_default_size (GTK_WINDOW (self), 1024, 768);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  self->release = gtk_button_new_with_label (_ ("Release Input"));
  gtk_widget_set_tooltip_text (self->release, _ ("Release keyboard and pointer (Ctrl+Alt)"));
  g_signal_connect (self->release, "clicked", G_CALLBACK (release_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->release);
  GtkWidget *fullscreen = gtk_button_new_from_icon_name ("view-fullscreen-symbolic");
  gtk_widget_set_tooltip_text (fullscreen, _ ("Toggle Fullscreen"));
  g_signal_connect (fullscreen, "clicked", G_CALLBACK (fullscreen_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), fullscreen);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  self->hint = GTK_LABEL (gtk_label_new (_ ("Click the display to send input to the machine")));
  gtk_widget_add_css_class (GTK_WIDGET (self->hint), "dim-label");
  gtk_widget_set_margin_top (GTK_WIDGET (self->hint), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->hint), 6);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->hint));
  self->stack = GTK_STACK (gtk_stack_new ());
  self->display = g_object_new (pirwa_display_get_type (), NULL);
  ((PirwaDisplay *)self->display)->console = self;
  gtk_stack_add_named (self->stack, self->display, "display");
  self->status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status, "video-display-symbolic");
  GtkWidget *reconnect = gtk_button_new_with_label (_ ("Reconnect"));
  gtk_widget_set_halign (reconnect, GTK_ALIGN_CENTER);
  g_signal_connect (reconnect, "clicked", G_CALLBACK (reconnect_clicked), self);
  adw_status_page_set_child (self->status, reconnect);
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->status), "status");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->stack));
  adw_window_set_content (ADW_WINDOW (self), toolbar);

  GtkEventController *key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (key_pressed), self);
  g_signal_connect (key, "key-released", G_CALLBACK (key_released), self);
  gtk_widget_add_controller (self->display, key);
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (pointer_motion), self);
  gtk_widget_add_controller (self->display, motion);
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (pointer_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (pointer_released), self);
  gtk_widget_add_controller (self->display, GTK_EVENT_CONTROLLER (click));
  GtkEventController *scroll
      = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll, "scroll", G_CALLBACK (pointer_scroll), self);
  gtk_widget_add_controller (self->display, scroll);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "leave", G_CALLBACK (focus_left), self);
  gtk_widget_add_controller (self->display, focus);
  g_signal_connect (self, "close-request", G_CALLBACK (close_requested), NULL);
  g_signal_connect (self, "notify::is-active", G_CALLBACK (active_changed), NULL);
}

void
pirwa_console_present (GtkApplication *application, const char *uri, const char *uuid,
                       const char *name)
{
  for (GList *item = gtk_application_get_windows (application); item; item = item->next)
    if (PIRWA_IS_CONSOLE_WINDOW (item->data))
      {
        PirwaConsole *other = item->data;
        if (!other->closed && g_strcmp0 (other->uuid, uuid) == 0
            && g_str_equal (pirwa_backend_get_uri (other->backend), uri))
          {
            gtk_window_present (GTK_WINDOW (other));
            return;
          }
      }
  PirwaConsole *self
      = g_object_new (PIRWA_TYPE_CONSOLE, "application", application, "title", name, NULL);
  self->uuid = g_strdup (uuid);
  self->running = TRUE;
  self->backend = pirwa_backend_new (uri);
  g_signal_connect_object (self->backend, "inventory", G_CALLBACK (inventory_changed), self, 0);
  gtk_window_present (GTK_WINDOW (self));
  start_session (self);
}
