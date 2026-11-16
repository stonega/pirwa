/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-console.h"
#include "config.h"
#include "pirwa-backend.h"
#include "pirwa-display.h"
#include "pirwa-frame.h"
#include "pirwa-preferences.h"
#include <glib/gi18n.h>
#include <math.h>
#include <unistd.h>

struct _PirwaConsole
{
  GtkBox parent_instance;
  PirwaBackend *backend;
  GCancellable *pending;
  SpiceSession *session;
  SpiceMainChannel *main;
  SpiceInputsChannel *inputs;
  SpiceChannel *display_channel;
  SpiceDisplayPrimary primary;
  cairo_rectangle_int_t monitor_area;
  int guest_width;
  int guest_height;
  gboolean initial_mode_sent;
  gboolean display_configured;
  gboolean auto_resize;
  int configured_width;
  int configured_height;
  GSettings *preferences;
  GtkWidget *display;
  GtkWidget *release;
  AdwToolbarView *toolbar;
  GtkWidget *header;
  GtkStack *stack;
  AdwStatusPage *status;
  GtkWindow *host;  /* Borrowed from the widget root. */
  GtkWindow *popup; /* Owns this widget while detached. */
  int popup_bounds_width;
  int popup_bounds_height;
  GdkTexture *preview;
  char *name;
  char *uuid;
  guint generation;
  guint reconnect_source;
  guint connect_source;
  guint activate_source;
  guint configure_source;
  guint fit_source;
  guint retry_delay;
  guint buttons;
  gboolean keys[512];
  gboolean input_ready;
  gboolean display_activated;
  gboolean captured;
  gboolean closed;
  gboolean running;
  gboolean have_pointer;
  double pointer_x;
  double pointer_y;
  double scroll_y;
};
G_DEFINE_FINAL_TYPE (PirwaConsole, pirwa_console, GTK_TYPE_BOX)

static void start_session (PirwaConsole *self);
static void disconnect_session (PirwaConsole *self);

static gboolean
fit_popup (gpointer data)
{
  PirwaConsole *self = data;
  self->fit_source = 0;
  if (!self->popup || self->closed || gtk_window_is_fullscreen (self->popup)
      || gtk_window_is_maximized (self->popup))
    return G_SOURCE_REMOVE;

  int width = self->monitor_area.width > 0 ? self->monitor_area.width : self->guest_width;
  int height = self->monitor_area.height > 0 ? self->monitor_area.height : self->guest_height;
  int minimum_width, header_height;
  gtk_widget_measure (GTK_WIDGET (self), GTK_ORIENTATION_HORIZONTAL, -1, &minimum_width, NULL, NULL,
                      NULL);
  gtk_widget_measure (self->header, GTK_ORIENTATION_VERTICAL, MAX (width, minimum_width), NULL,
                      &header_height, NULL, NULL);
  if (adw_toolbar_view_get_top_bar_height (self->toolbar) > 0)
    header_height = adw_toolbar_view_get_top_bar_height (self->toolbar);

  double scale = 1.0;
  /* Leave desktop space around the popup. GNOME automatically maximizes new
   * windows covering over 80% of the work area, losing the guest aspect fit. */
  if (self->popup_bounds_width > 0)
    scale = MIN (scale, self->popup_bounds_width * 0.88 / width);
  if (self->popup_bounds_height > header_height)
    scale = MIN (scale, MAX (1.0, self->popup_bounds_height * 0.88 - header_height) / height);
  gtk_window_set_default_size (self->popup, MAX (minimum_width, (int)(width * scale + 0.5)),
                               (int)(height * scale + 0.5) + header_height);
  return G_SOURCE_REMOVE;
}

static void
schedule_popup_fit (PirwaConsole *self)
{
  if (self->popup && !self->closed && !self->fit_source)
    self->fit_source = g_idle_add (fit_popup, self);
}

static void
popup_chrome_changed (AdwToolbarView *toolbar, GParamSpec *spec, PirwaConsole *self)
{
  (void)toolbar;
  (void)spec;
  if (!self->auto_resize)
    schedule_popup_fit (self);
}

static void
popup_bounds_changed (GdkToplevel *surface, GdkToplevelSize *size, PirwaConsole *self)
{
  (void)surface;
  int width, height;
  gdk_toplevel_size_get_bounds (size, &width, &height);
  if (width == self->popup_bounds_width && height == self->popup_bounds_height)
    return;
  self->popup_bounds_width = width;
  self->popup_bounds_height = height;
  /* Wait until GTK has finished computing this frame's window geometry. */
  schedule_popup_fit (self);
}

static void
show_status (PirwaConsole *self, const char *title, const char *description, gboolean connecting)
{
  adw_status_page_set_title (self->status, title);
  g_autofree char *escaped = g_markup_escape_text (description, -1);
  adw_status_page_set_description (self->status, escaped);
  GtkWidget *spinner = adw_status_page_get_child (self->status);
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), connecting);
  gtk_widget_set_visible (spinner, connecting);
  gtk_stack_set_visible_child_name (self->stack, "status");
}

static GdkToplevel *
toplevel (PirwaConsole *self)
{
  GdkSurface *surface = self->host ? gtk_native_get_surface (GTK_NATIVE (self->host)) : NULL;
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
}

static void
release_clicked (GtkButton *button, PirwaConsole *self)
{
  (void)button;
  release_input (self);
  if (self->popup)
    adw_toolbar_view_set_reveal_top_bars (self->toolbar, TRUE);
  gtk_widget_grab_focus (self->popup ? self->release : self->display);
}

static gboolean
initial_display_size (PirwaConsole *self)
{
  /* Wait for the host window, not a guest frame or display allocation. */
  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return FALSE;
  GdkToplevel *surface = toplevel (self);
  GdkMonitor *monitor
      = surface ? gdk_display_get_monitor_at_surface (
                      gdk_surface_get_display (GDK_SURFACE (surface)), GDK_SURFACE (surface))
                : NULL;
  if (!monitor)
    return FALSE;
  GdkRectangle bounds;
  gdk_monitor_get_geometry (monitor, &bounds);
  if (bounds.width <= 0 || bounds.height <= 0)
    return FALSE;
  double scale = gdk_monitor_get_scale (monitor);
  self->guest_width = (int)CLAMP (bounds.width * scale + 0.5, 320, 8192);
  self->guest_height = (int)CLAMP (bounds.height * scale + 0.5, 200, 8192);
  return TRUE;
}

static gboolean
configure_display (gpointer data)
{
  PirwaConsole *self = data;
  self->configure_source = 0;
  if (!self->closed && self->main && (!self->display_configured || self->auto_resize))
    {
      /* Match the host monitor once. Reconnects retain the last guest-selected
       * mode; resizing or reparenting the viewer only scales the image. */
      if (!self->initial_mode_sent && !initial_display_size (self))
        return G_SOURCE_REMOVE;
      int width = self->guest_width;
      int height = self->guest_height;
      if (self->auto_resize)
        {
          int viewport_width = gtk_widget_get_width (GTK_WIDGET (self->stack))
                               - gtk_widget_get_margin_start (self->display)
                               - gtk_widget_get_margin_end (self->display);
          int viewport_height = gtk_widget_get_height (GTK_WIDGET (self->stack))
                                - gtk_widget_get_margin_top (self->display)
                                - gtk_widget_get_margin_bottom (self->display);
          if (viewport_width >= 320 && viewport_height >= 200)
            {
              width = MIN (viewport_width, 8192);
              height = MIN (viewport_height, 8192);
            }
        }
      if (self->display_configured && width == self->configured_width
          && height == self->configured_height)
        return G_SOURCE_REMOVE;
      self->initial_mode_sent = TRUE;
      self->display_configured = TRUE;
      self->configured_width = width;
      self->configured_height = height;
      self->guest_width = width;
      self->guest_height = height;
      spice_main_channel_update_display_enabled (self->main, 0, TRUE, FALSE);
      spice_main_channel_update_display (self->main, 0, 0, 0, width, height, TRUE);
    }
  return G_SOURCE_REMOVE;
}

static void
schedule_display_configuration (PirwaConsole *self)
{
  if (self->closed || (self->display_configured && !self->auto_resize))
    return;
  g_clear_handle_id (&self->configure_source, g_source_remove);
  self->configure_source = g_timeout_add (200, configure_display, self);
}

static void
auto_resize_changed (GSettings *settings, const char *key, PirwaConsole *self)
{
  self->auto_resize = g_settings_get_boolean (settings, key);
  if (self->auto_resize)
    schedule_display_configuration (self);
  else
    {
      g_clear_handle_id (&self->configure_source, g_source_remove);
      if (!self->display_configured)
        schedule_display_configuration (self);
    }
}

static void
display_resized (PirwaDisplay *display, int width, int height, PirwaConsole *self)
{
  (void)display;
  (void)width;
  (void)height;
  if (self->auto_resize)
    schedule_display_configuration (self);
}

static void
set_display_inset (PirwaConsole *self, int inset)
{
  gtk_widget_set_margin_start (self->display, inset);
  gtk_widget_set_margin_end (self->display, inset);
  gtk_widget_set_margin_top (self->display, inset);
  gtk_widget_set_margin_bottom (self->display, inset);
}

static void
invalidate (SpiceChannel *channel, int x, int y, int width, int height, PirwaConsole *self)
{
  if (channel == self->display_channel)
    pirwa_display_invalidate (PIRWA_DISPLAY (self->display),
                              MAX ((gint64)x - self->monitor_area.x, G_MININT),
                              MAX ((gint64)y - self->monitor_area.y, G_MININT), width, height);
}

static void
primary_destroy (SpiceChannel *channel, PirwaConsole *self)
{
  if (channel != self->display_channel)
    return;
  memset (&self->primary, 0, sizeof self->primary);
  self->monitor_area = (cairo_rectangle_int_t){ 0 };
  pirwa_display_clear (PIRWA_DISPLAY (self->display));
}

static gboolean
update_monitor_area (PirwaConsole *self)
{
  g_autoptr (GArray) monitors = NULL;
  SpiceDisplayMonitorConfig *monitor = NULL;
  g_object_get (self->display_channel, "monitors", &monitors, NULL);
  for (guint i = 0; monitors && i < monitors->len; i++)
    {
      SpiceDisplayMonitorConfig *candidate
          = &g_array_index (monitors, SpiceDisplayMonitorConfig, i);
      if (candidate->id == 0)
        {
          monitor = candidate;
          break;
        }
    }
  /* A single head's surface starts at zero, even if its desktop position does not. */
  SpiceDisplayMonitorConfig single;
  if (monitor && monitors->len == 1)
    {
      single = *monitor;
      single.x = single.y = 0;
      monitor = &single;
    }
  cairo_rectangle_int_t area = pirwa_frame_monitor_area (&self->primary, monitor);
  if (area.width == 0 || area.height == 0)
    return FALSE;
  if (monitor && (guint)area.width == monitor->width && (guint)area.height == monitor->height
      && area.width >= 320 && area.height >= 200)
    {
      self->guest_width = area.width;
      self->guest_height = area.height;
    }
  if (area.x == self->monitor_area.x && area.y == self->monitor_area.y
      && area.width == self->monitor_area.width && area.height == self->monitor_area.height)
    return TRUE;
  gboolean resized
      = area.width != self->monitor_area.width || area.height != self->monitor_area.height;
  self->monitor_area = area;
  SpiceDisplayPrimary visible = self->primary;
  int pixel_size
      = visible.format == SPICE_SURFACE_FMT_32_xRGB || visible.format == SPICE_SURFACE_FMT_32_ARGB
            ? 4
            : 2;
  visible.data += (gsize)area.y * visible.stride + (gsize)area.x * pixel_size;
  visible.width = area.width;
  visible.height = area.height;
  gboolean valid = pirwa_display_set_primary (PIRWA_DISPLAY (self->display), &visible);
  /* With automatic guest adjustment enabled, the viewport drives the guest,
   * rather than feeding its response back into the window size. */
  if (valid && resized && !self->auto_resize)
    schedule_popup_fit (self);
  return valid;
}

static void
monitor_changed (SpiceChannel *channel, GParamSpec *spec, PirwaConsole *self)
{
  (void)spec;
  if (channel == self->display_channel && self->primary.data)
    update_monitor_area (self);
}

static void
primary_create (SpiceChannel *channel, int format, int width, int height, int stride, int shmid,
                gpointer data, PirwaConsole *self)
{
  g_clear_handle_id (&self->connect_source, g_source_remove);
  g_clear_handle_id (&self->activate_source, g_source_remove);
  g_clear_handle_id (&self->reconnect_source, g_source_remove);
  primary_destroy (channel, self);
  self->primary = (SpiceDisplayPrimary){ .format = format,
                                         .width = width,
                                         .height = height,
                                         .stride = stride,
                                         .shmid = shmid,
                                         .data = data };
  /* Validate before exposing the display; malformed/unimplemented formats are
   * reported rather than leaving a permanently blank window. */
  if (!update_monitor_area (self))
    {
      show_status (self, _ ("Unsupported display"),
                   _ ("This console supports RGB displays up to 8192 × 8192 pixels."), FALSE);
      return;
    }
  self->retry_delay = 0;
  gtk_spinner_stop (GTK_SPINNER (adw_status_page_get_child (self->status)));
  gtk_stack_set_visible_child_name (self->stack, "display");
  schedule_display_configuration (self);
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
  if (!self->closed && self->running)
    start_session (self);
  return G_SOURCE_REMOVE;
}

static void
schedule_reconnect (PirwaConsole *self, const char *reason)
{
  if (self->closed || !self->running || self->reconnect_source)
    return;
  g_clear_handle_id (&self->connect_source, g_source_remove);
  release_input (self);
  g_autofree char *description
      = reason ? g_strdup_printf (_ ("%s\nReconnecting automatically…"), reason)
               : g_strdup (_ ("Reconnecting automatically…"));
  show_status (self, _ ("Connecting to display"), description, TRUE);
  self->retry_delay = self->retry_delay ? MIN (self->retry_delay * 2, 16u) : 1;
  /* One pending attempt per console, with bounded backoff while the VM runs. */
  self->reconnect_source = g_timeout_add_seconds (self->retry_delay, retry_session, self);
}

static gboolean
first_frame_timeout (gpointer data)
{
  PirwaConsole *self = data;
  self->connect_source = 0;
  schedule_reconnect (self, _ ("The machine has not sent a display frame yet."));
  return G_SOURCE_REMOVE;
}

static gboolean
activate_display (gpointer data)
{
  PirwaConsole *self = data;
  self->activate_source = 0;
  if (self->closed || self->primary.data || !self->input_ready || !self->inputs
      || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;
  int width = gtk_widget_get_width (GTK_WIDGET (self->stack));
  int height = gtk_widget_get_height (GTK_WIDGET (self->stack));
  if (width < 320 || height < 200)
    return G_SOURCE_REMOVE;
  /* Some idle QXL guests send no primary surface until pointer activity.
   * Two adjacent positions guarantee motion without clicking or pressing keys.
   * Do this once per connection, only when its first frame is still missing. */
  int x = self->guest_width / 2;
  int y = self->guest_height / 2;
  self->display_activated = TRUE;
  spice_inputs_channel_position (self->inputs, x - 1, y, 0, 0);
  spice_inputs_channel_position (self->inputs, x, y, 0, 0);
  return G_SOURCE_REMOVE;
}

static void
schedule_display_activation (PirwaConsole *self)
{
  if (!self->closed && !self->primary.data && self->input_ready && !self->display_activated
      && !self->activate_source && gtk_widget_get_mapped (GTK_WIDGET (self)))
    self->activate_source = g_timeout_add (500, activate_display, self);
}

static void
channel_event (SpiceChannel *channel, SpiceChannelEvent event, PirwaConsole *self)
{
  if (channel == SPICE_CHANNEL (self->inputs))
    {
      self->input_ready = event == SPICE_CHANNEL_OPENED;
      if (self->input_ready)
        schedule_display_activation (self);
    }
  if (event == SPICE_CHANNEL_OPENED || event == SPICE_CHANNEL_SWITCHING)
    {
      if (event == SPICE_CHANNEL_OPENED && SPICE_IS_MAIN_CHANNEL (channel))
        spice_main_channel_request_mouse_mode (SPICE_MAIN_CHANNEL (channel),
                                               SPICE_MOUSE_MODE_CLIENT);
      return;
    }
  if ((SPICE_IS_MAIN_CHANNEL (channel) || channel == self->display_channel) && !self->closed)
    {
      const GError *error = spice_channel_get_error (channel);
      /* Disconnect outside the SPICE signal/coroutine. Lifecycle events handle
       * stopped guests; transient disconnects reconnect automatically. */
      schedule_reconnect (self, error ? error->message : NULL);
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
            {
              spice_channel_disconnect (request->channel, SPICE_CHANNEL_NONE);
              schedule_reconnect (self, NULL);
            }
        }
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          spice_channel_disconnect (request->channel, SPICE_CHANNEL_NONE);
          /* NONE suppresses channel-event, so this path must schedule retry. */
          schedule_reconnect (self, error->message);
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
  schedule_display_configuration (self);
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
      schedule_display_configuration (self);
      return; /* spice_session_open_fd connects the main channel. */
    }
  if (SPICE_IS_DISPLAY_CHANNEL (channel))
    {
      self->display_channel = channel;
      g_signal_connect_object (channel, "notify::monitors", G_CALLBACK (monitor_changed), self, 0);
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
  g_clear_handle_id (&self->connect_source, g_source_remove);
  g_clear_handle_id (&self->activate_source, g_source_remove);
  self->display_activated = FALSE;
  self->display_configured = FALSE;
  g_clear_handle_id (&self->configure_source, g_source_remove);
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
  if (self->closed || !self->running || self->session || self->reconnect_source)
    return;
  self->pending = g_cancellable_new ();
  self->session = spice_session_new ();
  g_object_set (self->session, "enable-usbredir", FALSE, "enable-smartcard", FALSE, NULL);
  g_signal_connect_object (self->session, "channel-new", G_CALLBACK (channel_new), self, 0);
  g_signal_connect_object (self->session, "channel-destroy", G_CALLBACK (channel_destroy), self, 0);
  spice_audio_get (self->session, NULL);
  show_status (self, _ ("Connecting to display"), _ ("Waiting for the machine’s first frame…"),
               TRUE);
  self->connect_source = g_timeout_add_seconds (15, first_frame_timeout, self);
  if (!spice_session_open_fd (self->session, -1))
    schedule_reconnect (self, _ ("Cannot open display"));
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
      self->retry_delay = 0;
      show_status (self, _ ("Machine is stopped"), _ ("The display will reconnect when it starts."),
                   FALSE);
    }
  else if (!self->session && !self->reconnect_source)
    {
      self->retry_delay = 0;
      start_session (self);
    }
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
                             gtk_widget_get_height (self->display), self->monitor_area.width,
                             self->monitor_area.height, x, y, &guest_x, &guest_y))
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
active_changed (GtkWindow *window, GParamSpec *spec, PirwaConsole *self)
{
  (void)spec;
  if (!gtk_window_is_active (window))
    release_input (self);
}

static void
fullscreen_clicked (GtkButton *button, PirwaConsole *self)
{
  (void)button;
  if (!self->popup)
    return;
  if (gtk_window_is_fullscreen (self->popup))
    gtk_window_unfullscreen (self->popup);
  else
    gtk_window_fullscreen (self->popup);
}

static void
fullscreen_changed (GtkWindow *window, GParamSpec *spec, PirwaConsole *self)
{
  (void)spec;
  gboolean fullscreen = self->popup && gtk_window_is_fullscreen (window);
  pirwa_display_set_rounded (PIRWA_DISPLAY (self->display), !fullscreen);
  adw_toolbar_view_set_top_bar_style (self->toolbar,
                                      fullscreen ? ADW_TOOLBAR_RAISED : ADW_TOOLBAR_FLAT);
  adw_toolbar_view_set_extend_content_to_top_edge (self->toolbar, !self->popup || fullscreen);
  adw_toolbar_view_set_reveal_top_bars (self->toolbar, self->popup && !fullscreen);
  if (!self->auto_resize)
    schedule_popup_fit (self);
}

static void
pirwa_console_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (pirwa_console_parent_class)->map (widget);
  /* Agent/channel events can precede the first usable viewport allocation. */
  PirwaConsole *self = PIRWA_CONSOLE_VIEW (widget);
  schedule_display_configuration (self);
  schedule_display_activation (self);
}

static void
pirwa_console_root (GtkWidget *widget)
{
  PirwaConsole *self = PIRWA_CONSOLE_VIEW (widget);
  GTK_WIDGET_CLASS (pirwa_console_parent_class)->root (widget);
  GtkRoot *root = gtk_widget_get_root (widget);
  if (GTK_IS_WINDOW (root))
    {
      self->host = GTK_WINDOW (root);
      g_signal_connect_object (root, "notify::is-active", G_CALLBACK (active_changed), self, 0);
      g_signal_connect_object (root, "notify::fullscreened", G_CALLBACK (fullscreen_changed), self,
                               0);
      g_signal_connect_object (root, "notify::maximized", G_CALLBACK (fullscreen_changed), self, 0);
      fullscreen_changed (self->host, NULL, self);
    }
}

static void
pirwa_console_unroot (GtkWidget *widget)
{
  PirwaConsole *self = PIRWA_CONSOLE_VIEW (widget);
  release_input (self);
  if (self->host)
    g_signal_handlers_disconnect_by_data (self->host, self);
  self->host = NULL;
  GTK_WIDGET_CLASS (pirwa_console_parent_class)->unroot (widget);
}

static void
toolbar_motion (GtkEventControllerMotion *controller, double x, double y, PirwaConsole *self)
{
  (void)controller;
  (void)x;
  if (!self->popup || !gtk_window_is_fullscreen (self->popup))
    return;
  if (y <= 3)
    adw_toolbar_view_set_reveal_top_bars (self->toolbar, TRUE);
  else if (y > gtk_widget_get_height (self->header))
    adw_toolbar_view_set_reveal_top_bars (self->toolbar, FALSE);
}

static void
toolbar_left (GtkEventControllerMotion *controller, PirwaConsole *self)
{
  (void)controller;
  if (self->popup && gtk_window_is_fullscreen (self->popup))
    adw_toolbar_view_set_reveal_top_bars (self->toolbar, FALSE);
}

static void
stop_console (PirwaConsole *self)
{
  if (self->closed)
    return;
  self->closed = TRUE;
  g_clear_handle_id (&self->fit_source, g_source_remove);
  g_clear_handle_id (&self->reconnect_source, g_source_remove);
  g_clear_handle_id (&self->configure_source, g_source_remove);
  disconnect_session (self);
  if (self->backend)
    pirwa_backend_stop (self->backend);
}

static gboolean
close_requested (GtkWindow *window, PirwaConsole *self)
{
  (void)window;
  g_signal_emit_by_name (self, "return-requested");
  if (!self->popup)
    return TRUE;
  self->popup = NULL;
  stop_console (self);
  return FALSE;
}

static void
pirwa_console_dispose (GObject *object)
{
  PirwaConsole *self = PIRWA_CONSOLE_VIEW (object);
  stop_console (self);
  g_clear_object (&self->backend);
  g_clear_object (&self->preferences);
  g_clear_object (&self->preview);
  G_OBJECT_CLASS (pirwa_console_parent_class)->dispose (object);
}

static void
pirwa_console_finalize (GObject *object)
{
  g_free (PIRWA_CONSOLE_VIEW (object)->uuid);
  g_free (PIRWA_CONSOLE_VIEW (object)->name);
  G_OBJECT_CLASS (pirwa_console_parent_class)->finalize (object);
}

static void
pirwa_console_class_init (PirwaConsoleClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pirwa_console_dispose;
  G_OBJECT_CLASS (klass)->finalize = pirwa_console_finalize;
  GTK_WIDGET_CLASS (klass)->map = pirwa_console_map;
  GTK_WIDGET_CLASS (klass)->root = pirwa_console_root;
  GTK_WIDGET_CLASS (klass)->unroot = pirwa_console_unroot;
  g_signal_new ("return-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                NULL, G_TYPE_NONE, 0);
}

static void
pirwa_console_init (PirwaConsole *self)
{
  self->preferences = pirwa_preferences_new ();
  self->auto_resize = g_settings_get_boolean (self->preferences, "auto-resize-display");
  g_signal_connect_object (self->preferences, "changed::auto-resize-display",
                           G_CALLBACK (auto_resize_changed), self, 0);
  /* Placeholder for popup fitting before mapping selects the host monitor. */
  self->guest_width = 1280;
  self->guest_height = 720;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  self->toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  g_signal_connect_object (self->toolbar, "notify::top-bar-height",
                           G_CALLBACK (popup_chrome_changed), self, 0);
  self->header = adw_header_bar_new ();
  self->release = gtk_button_new_with_label (_ ("Release Input"));
  gtk_widget_set_tooltip_text (self->release, _ ("Release keyboard and pointer (Ctrl+Alt)"));
  g_signal_connect (self->release, "clicked", G_CALLBACK (release_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (self->header), self->release);
  GtkWidget *fullscreen = gtk_button_new_from_icon_name ("view-fullscreen-symbolic");
  gtk_widget_set_tooltip_text (fullscreen, _ ("Toggle Fullscreen"));
  g_signal_connect (fullscreen, "clicked", G_CALLBACK (fullscreen_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (self->header), fullscreen);
  adw_toolbar_view_add_top_bar (self->toolbar, self->header);
  self->stack = GTK_STACK (gtk_stack_new ());
  self->display = pirwa_display_new ();
  set_display_inset (self, 6);
  g_signal_connect_object (self->display, "resize", G_CALLBACK (display_resized), self, 0);
  gtk_stack_add_named (self->stack, self->display, "display");
  self->status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status, "video-display-symbolic");
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  adw_status_page_set_child (self->status, spinner);
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->status), "status");
  adw_toolbar_view_set_content (self->toolbar, GTK_WIDGET (self->stack));
  gtk_widget_set_vexpand (GTK_WIDGET (self->toolbar), TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toolbar));
  adw_toolbar_view_set_extend_content_to_top_edge (self->toolbar, TRUE);
  adw_toolbar_view_set_reveal_top_bars (self->toolbar, FALSE);

  GtkEventController *toolbar_pointer = gtk_event_controller_motion_new ();
  gtk_event_controller_set_propagation_phase (toolbar_pointer, GTK_PHASE_CAPTURE);
  g_signal_connect (toolbar_pointer, "enter", G_CALLBACK (toolbar_motion), self);
  g_signal_connect (toolbar_pointer, "motion", G_CALLBACK (toolbar_motion), self);
  g_signal_connect (toolbar_pointer, "leave", G_CALLBACK (toolbar_left), self);
  gtk_widget_add_controller (GTK_WIDGET (self->toolbar), toolbar_pointer);

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
}

PirwaConsole *
pirwa_console_new (const char *uri, const char *uuid, const char *name)
{
  PirwaConsole *self = g_object_new (PIRWA_TYPE_CONSOLE, NULL);
  self->uuid = g_strdup (uuid);
  self->name = g_strdup (name);
  self->running = TRUE;
  self->backend = pirwa_backend_new (uri);
  g_signal_connect_object (self->backend, "inventory", G_CALLBACK (inventory_changed), self, 0);
  start_session (self);
  return self;
}

gboolean
pirwa_console_matches (PirwaConsole *self, const char *uri, const char *uuid)
{
  return !self->closed && g_strcmp0 (self->uuid, uuid) == 0
         && g_str_equal (pirwa_backend_get_uri (self->backend), uri);
}

PirwaConsole *
pirwa_console_find_detached (GtkApplication *application, const char *uri, const char *uuid)
{
  for (GList *item = gtk_application_get_windows (application); item; item = item->next)
    {
      PirwaConsole *console = g_object_get_data (G_OBJECT (item->data), "pirwa-console");
      if (console && pirwa_console_matches (console, uri, uuid))
        return console;
    }
  return NULL;
}

gboolean
pirwa_console_is_detached (PirwaConsole *self)
{
  return self->popup != NULL;
}

GdkPaintable *
pirwa_console_get_preview (PirwaConsole *self)
{
  return self->preview ? GDK_PAINTABLE (self->preview) : NULL;
}

void
pirwa_console_release_input (PirwaConsole *self)
{
  release_clicked (NULL, self);
}

void
pirwa_console_pop_out (PirwaConsole *self, GtkApplication *application)
{
  g_return_if_fail (gtk_widget_get_parent (GTK_WIDGET (self)) == NULL);
  g_return_if_fail (self->popup == NULL);
  release_input (self);
  g_clear_object (&self->preview);
  if (self->primary.data)
    self->preview = pirwa_frame_texture_rect (&self->primary, &self->monitor_area);
  set_display_inset (self, 0);
  self->popup = GTK_WINDOW (adw_window_new ());
  gtk_window_set_application (self->popup, application);
  gtk_window_set_title (self->popup, self->name);
  g_object_set_data (G_OBJECT (self->popup), "pirwa-console", self);
  g_signal_connect_object (self->popup, "close-request", G_CALLBACK (close_requested), self, 0);
  adw_window_set_content (ADW_WINDOW (self->popup), GTK_WIDGET (self));
  gtk_widget_realize (GTK_WIDGET (self->popup));
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (self->popup));
  GdkMonitor *monitor
      = gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface);
  GdkRectangle bounds = { 0 };
  if (monitor)
    gdk_monitor_get_geometry (monitor, &bounds);
  self->popup_bounds_width = bounds.width;
  self->popup_bounds_height = bounds.height;
  g_signal_connect_object (surface, "compute-size", G_CALLBACK (popup_bounds_changed), self,
                           G_CONNECT_AFTER);
  g_clear_handle_id (&self->fit_source, g_source_remove);
  fit_popup (self);
  gtk_window_present (self->popup);
}

void
pirwa_console_return (PirwaConsole *self)
{
  g_return_if_fail (self->popup != NULL);
  release_input (self);
  GtkWindow *popup = self->popup;
  self->popup = NULL;
  g_clear_handle_id (&self->fit_source, g_source_remove);
  pirwa_display_set_rounded (PIRWA_DISPLAY (self->display), TRUE);
  set_display_inset (self, 6);
  adw_window_set_content (ADW_WINDOW (popup), NULL);
  g_object_set_data (G_OBJECT (popup), "pirwa-console", NULL);
  gtk_window_destroy (popup);
  adw_toolbar_view_set_reveal_top_bars (self->toolbar, FALSE);
  adw_toolbar_view_set_extend_content_to_top_edge (self->toolbar, TRUE);
  adw_toolbar_view_set_top_bar_style (self->toolbar, ADW_TOOLBAR_FLAT);
}
