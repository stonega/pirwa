/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit desktop QA. Synthetic frames and test:///default only. */
#include "pirwa-backend.h"
#include "pirwa-console.h"
#include "pirwa-display.h"
#include "pirwa-preferences.h"
#include <spice-client.h>

static const char *uuid = "6695eb01-f6a4-8304-79aa-97f2502e193f";
static guint attempts;
static guint socket_errors;
static guint retry_delays[32];
static guint n_delays;
static gboolean fail_connections;
static PirwaBackend *backend; /* Borrowed while the console is alive. */
static SpiceChannel *display;
static SpiceMainChannel *main_channel;
static gboolean bootstrap;
static gboolean display_enabled;
static gboolean needs_activation;
static guint pointer_positions;
static int pointer_x, pointer_y;
static SpiceInputsChannel *inputs_channel;
static guint monitor_updates;
static int requested_width, requested_height;
static GtkStack *bootstrap_stack;
static SpiceDisplayPrimary visible_frame;
static GtkWindow *host_window;
static gboolean host_active = TRUE;
static gboolean host_suspended;

gboolean __real_gtk_window_is_active (GtkWindow *window);
gboolean __real_gtk_window_is_suspended (GtkWindow *window);

gboolean
__wrap_gtk_window_is_active (GtkWindow *window)
{
  return window == host_window ? host_active : __real_gtk_window_is_active (window);
}

gboolean
__wrap_gtk_window_is_suspended (GtkWindow *window)
{
  return window == host_window ? host_suspended : __real_gtk_window_is_suspended (window);
}

gboolean __real_pirwa_display_set_primary (PirwaDisplay *self, const SpiceDisplayPrimary *primary);

gboolean
__wrap_pirwa_display_set_primary (PirwaDisplay *self, const SpiceDisplayPrimary *primary)
{
  visible_frame = *primary;
  return __real_pirwa_display_set_primary (self, primary);
}

void __real_spice_main_channel_update_display_enabled (SpiceMainChannel *channel, int id,
                                                       gboolean enabled, gboolean update);
void __real_spice_main_channel_update_display (SpiceMainChannel *channel, int id, int x, int y,
                                               int width, int height, gboolean update);

gboolean __real_spice_session_open_fd (SpiceSession *session, int fd);
PirwaReply *__real_pirwa_backend_request_finish (PirwaBackend *self, GAsyncResult *result,
                                                 GError **error);

guint
__wrap_g_timeout_add_seconds (guint interval, GSourceFunc function, gpointer data)
{
  /* Keep the production delays observable but make this deterministic fixture fast. */
  if (interval != 15)
    {
      g_assert_cmpuint (n_delays, <, G_N_ELEMENTS (retry_delays));
      retry_delays[n_delays++] = interval;
    }
  /* Window bootstrap includes cold renderer setup, especially under ASan.
   * Keep its real watchdog deadline; transport-only cases still run quickly. */
  return g_timeout_add (interval == 15 ? (bootstrap ? 15000 : 250) : 10, function, data);
}

PirwaReply *
__wrap_pirwa_backend_request_finish (PirwaBackend *self, GAsyncResult *result, GError **error)
{
  PirwaReply *reply = __real_pirwa_backend_request_finish (self, result, error);
  backend = self;
  if (!reply && error && *error)
    socket_errors++;
  return reply;
}

gboolean
__wrap_spice_channel_connect (SpiceChannel *channel)
{
  (void)channel;
  return TRUE; /* Synthetic display channels need no socket. */
}

static gboolean
send_frame (gpointer data)
{
  static guint32 pixels[16];
  memset (pixels, 0, sizeof pixels);
  g_signal_emit_by_name (data, "display-primary-create", SPICE_SURFACE_FMT_32_xRGB, 4, 4, 16, 0,
                         pixels);
  return G_SOURCE_REMOVE;
}

void
__wrap_spice_main_channel_update_display_enabled (SpiceMainChannel *channel, int id,
                                                  gboolean enabled, gboolean update)
{
  if (!bootstrap)
    {
      __real_spice_main_channel_update_display_enabled (channel, id, enabled, update);
      return;
    }
  g_assert_cmpint (id, ==, 0);
  g_assert_true (enabled);
  display_enabled = TRUE;
}

void
__wrap_spice_main_channel_update_display (SpiceMainChannel *channel, int id, int x, int y,
                                          int width, int height, gboolean update)
{
  if (!bootstrap)
    {
      __real_spice_main_channel_update_display (channel, id, x, y, width, height, update);
      return;
    }
  g_assert_cmpint (id, ==, 0);
  g_assert_true (display_enabled);
  g_assert_true (update);
  g_assert_cmpint (width, >=, 320);
  g_assert_cmpint (height, >=, 200);
  requested_width = width;
  requested_height = height;
  if (monitor_updates++ == 0)
    {
      /* Model a disabled guest monitor: it cannot send a primary surface until
       * the client enables it and supplies a size, while the status is visible. */
      g_assert_cmpstr (gtk_stack_get_visible_child_name (bootstrap_stack), ==, "status");
      if (!needs_activation)
        send_frame (display); /* Deliver the healthy frame before any due activation timer. */
    }
}

void
__wrap_spice_inputs_channel_position (SpiceInputsChannel *channel, int x, int y, int display_id,
                                      int button_state)
{
  g_assert_true (bootstrap);
  g_assert_true (needs_activation);
  g_assert_true (channel == inputs_channel);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (bootstrap_stack), ==, "status");
  g_assert_cmpuint (monitor_updates, >=, 1);
  g_assert_cmpint (display_id, ==, 0);
  g_assert_cmpint (button_state, ==, 0);
  if (pointer_positions++ % 2 == 0)
    {
      pointer_x = x;
      pointer_y = y;
    }
  else
    {
      g_assert_cmpint (x, ==, pointer_x + 1);
      g_assert_cmpint (y, ==, pointer_y);
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, send_frame, g_object_ref (display), g_object_unref);
    }
}

gboolean
__wrap_spice_session_open_fd (SpiceSession *session, int fd)
{
  g_assert_cmpint (fd, ==, -1);
  attempts++;
  if (bootstrap)
    {
      SpiceChannel *main = spice_channel_new (session, SPICE_CHANNEL_MAIN, 0);
      g_set_object (&main_channel, SPICE_MAIN_CHANNEL (main));
      SpiceChannel *channel = spice_channel_new (session, SPICE_CHANNEL_DISPLAY, 0);
      g_set_object (&display, channel);
      SpiceChannel *inputs = spice_channel_new (session, SPICE_CHANNEL_INPUTS, 0);
      g_set_object (&inputs_channel, SPICE_INPUTS_CHANNEL (inputs));
      g_signal_emit_by_name (inputs, "channel-event", SPICE_CHANNEL_OPENED);
      g_signal_emit_by_name (main, "main-agent-update");
      return TRUE;
    }
  if (fail_connections || attempts == 1 || attempts == 5 || attempts == 6)
    return FALSE;
  if (attempts == 2)
    return TRUE; /* Initiated, but never receives a first frame. */
  if (attempts == 3)
    return __real_spice_session_open_fd (session, fd); /* Test driver rejects the socket. */
  if (attempts == 4)
    {
      SpiceChannel *main = spice_channel_new (session, SPICE_CHANNEL_MAIN, 0);
      g_signal_emit_by_name (main, "channel-event", SPICE_CHANNEL_ERROR_CONNECT);
      g_signal_emit_by_name (main, "channel-event", SPICE_CHANNEL_CLOSED);
      return TRUE;
    }
  SpiceChannel *channel = spice_channel_new (session, SPICE_CHANNEL_DISPLAY, 0);
  g_set_object (&display, channel);
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, send_frame, g_object_ref (channel), g_object_unref);
  return TRUE;
}

static GtkWidget *
find_type (GtkWidget *widget, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_type (child, type);
      if (found)
        return found;
    }
  return NULL;
}

static void
iterate_for (guint milliseconds)
{
  gint64 deadline = g_get_monotonic_time () + milliseconds * G_TIME_SPAN_MILLISECOND;
  while (g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
}

static void
wait_for_display (GtkStack *stack)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_strcmp0 (gtk_stack_get_visible_child_name (stack), "display") != 0)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      iterate_for (1);
    }
}

static void
inventory_state (int state)
{
  g_assert_nonnull (backend);
  g_autoptr (GPtrArray) machines = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (machines, pirwa_machine_new_full (uuid, "test", "Test state", state));
  PirwaReply reply = { .machines = machines };
  g_signal_emit_by_name (backend, "inventory", &reply);
}

static void
assert_display_corner (GtkWidget *guest, gboolean rounded)
{
  int width = gtk_widget_get_width (guest), height = gtk_widget_get_height (guest);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GTK_WIDGET_GET_CLASS (guest)->snapshot (guest, snapshot);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, width, height);
  GskRenderer *renderer = gtk_native_get_renderer (gtk_widget_get_native (guest));
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, &bounds);
  g_autofree guchar *pixels = g_malloc_n ((gsize)width * height, 4);
  gdk_texture_download (texture, pixels, (gsize)width * 4);
  double scale = MIN ((double)width / visible_frame.width, (double)height / visible_frame.height);
  int x = (width - visible_frame.width * scale) / 2 + 1;
  int y = (height - visible_frame.height * scale) / 2 + 1;
  guchar alpha = pixels[((gsize)y * width + x) * 4 + 3];
  g_assert_cmpuint (alpha, ==, rounded ? 0 : 255);
}

static void
assert_display_color (GtkWidget *guest, guint32 color)
{
  int width = gtk_widget_get_width (guest), height = gtk_widget_get_height (guest);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GTK_WIDGET_GET_CLASS (guest)->snapshot (guest, snapshot);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, width, height);
  GskRenderer *renderer = gtk_native_get_renderer (gtk_widget_get_native (guest));
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, &bounds);
  g_autofree guchar *pixels = g_malloc_n ((gsize)width * height, 4);
  gdk_texture_download (texture, pixels, (gsize)width * 4);
  guchar *center = pixels + ((gsize)(height / 2) * width + width / 2) * 4;
  g_assert_cmpuint (center[0], ==, color & 255);
  g_assert_cmpuint (center[1], ==, (color >> 8) & 255);
  g_assert_cmpuint (center[2], ==, (color >> 16) & 255);
  g_assert_cmpuint (center[3], ==, 255);
}

static void
check_reactivation (PirwaConsole *console, GtkWindow *window)
{
  GtkWidget *guest = find_type (GTK_WIDGET (console), PIRWA_TYPE_DISPLAY);
  guint connections = attempts, updates = monitor_updates, positions = pointer_positions;
  assert_display_color (guest, 0);

  const guint32 colors[] = { 0x0020c060, 0x00e04080, 0x003060e0, 0x00b08030 };
  for (guint cycle = 0; cycle < G_N_ELEMENTS (colors); cycle++)
    {
      /* Inject window-manager state so the private desktop's focus policy
       * cannot make this signal-path and pixel regression nondeterministic. */
      host_active = FALSE;
      g_object_notify (G_OBJECT (window), "is-active");
      if (cycle == 2)
        {
          host_suspended = TRUE;
          g_object_notify (G_OBJECT (window), "suspended");
        }
      if (cycle == 3)
        gtk_widget_set_visible (GTK_WIDGET (console), FALSE);
      /* Model a stale cached frame after inactivity, with no new damage event
       * on return. Refresh must copy the current primary, not just queue GTK. */
      guint32 color = colors[cycle];
      for (int y = 0; y < visible_frame.height; y++)
        for (int x = 0; x < visible_frame.width; x++)
          ((guint32 *)(visible_frame.data + y * visible_frame.stride))[x] = color;
      host_active = TRUE;
      if (cycle < 2)
        g_object_notify (G_OBJECT (window), "is-active");
      else if (cycle == 2)
        {
          host_suspended = FALSE;
          g_object_notify (G_OBJECT (window), "suspended");
        }
      else
        gtk_widget_set_visible (GTK_WIDGET (console), TRUE);
      assert_display_color (guest, color);
    }
  g_assert_cmpuint (attempts, ==, connections);
  g_assert_cmpuint (monitor_updates, ==, updates);
  g_assert_cmpuint (pointer_positions, ==, positions);

  gboolean was_activation = needs_activation;
  needs_activation = TRUE;
  for (guint cycle = 0; cycle < 2; cycle++)
    {
      if (cycle == 0)
        {
          host_active = FALSE;
          g_object_notify (G_OBJECT (window), "is-active");
        }
      else
        {
          host_suspended = TRUE;
          g_object_notify (G_OBJECT (window), "suspended");
        }
      g_signal_emit_by_name (display, "display-primary-destroy");
      g_assert_cmpstr (gtk_stack_get_visible_child_name (bootstrap_stack), ==, "status");
      iterate_for (600);
      g_assert_cmpuint (pointer_positions, ==, positions);
      if (cycle == 0)
        {
          host_active = TRUE;
          g_object_notify (G_OBJECT (window), "is-active");
        }
      else
        {
          host_suspended = FALSE;
          g_object_notify (G_OBJECT (window), "suspended");
        }
      wait_for_display (bootstrap_stack);
      positions += 2;
      g_assert_cmpuint (pointer_positions, ==, positions);
      assert_display_color (guest, 0);
      iterate_for (600);
      g_assert_cmpuint (pointer_positions, ==, positions);
      g_assert_cmpuint (attempts, ==, connections);
      g_assert_cmpuint (monitor_updates, ==, updates);
    }
  needs_activation = was_activation;
}

static void
check_popup_fit (PirwaConsole *console, GtkWindow *embedded)
{
  g_autoptr (GArray) monitors = NULL;
  g_object_get (display, "monitors", &monitors, NULL);
  g_array_set_size (monitors, 1);
  SpiceDisplayMonitorConfig *monitor = &g_array_index (monitors, SpiceDisplayMonitorConfig, 0);
  *monitor = (SpiceDisplayMonitorConfig){ .width = 1280, .height = 720 };
  /* The visible monitor, rather than the larger backing surface, sizes the popup. */
  g_autofree guint32 *pixels = g_new0 (guint32, 1920 * 1200);
  g_signal_emit_by_name (display, "display-primary-create", SPICE_SURFACE_FMT_32_xRGB, 1920, 1200,
                         1920 * 4, 0, pixels);
  guint updates = monitor_updates;
  adw_window_set_content (ADW_WINDOW (embedded), NULL);
  pirwa_console_pop_out (console, NULL);
  GtkWindow *popup = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (console)));
  GtkWidget *guest = find_type (GTK_WIDGET (console), PIRWA_TYPE_DISPLAY);
  iterate_for (500);
  int width = gtk_widget_get_width (guest), height = gtk_widget_get_height (guest);
  g_assert_cmpint (width, >, 0);
  g_assert_false (gtk_window_is_maximized (popup));
  g_assert_cmpint (height, >, 0);
  g_assert_cmpint (width, <=, 1280);
  g_assert_cmpint (height, <=, 720);
  g_assert_cmpint (ABS (width * 720 - height * 1280), <=, 1280);
  g_assert_cmpuint (monitor_updates, ==, updates);

  monitor->width = 640;
  monitor->height = 480;
  g_object_notify (G_OBJECT (display), "monitors");
  iterate_for (400);
  width = gtk_widget_get_width (guest);
  height = gtk_widget_get_height (guest);
  g_assert_cmpint (width, <=, 640);
  g_assert_cmpint (height, <=, 480);
  g_assert_cmpint (ABS (width * 480 - height * 640), <=, 640);
  g_assert_cmpint (gtk_widget_get_height (GTK_WIDGET (popup)), >, height);
  assert_display_corner (guest, TRUE);

  /* Repeated geometry notifications must preserve a manual window resize. */
  gtk_window_set_default_size (popup, 700, 550);
  iterate_for (300);
  g_object_notify (G_OBJECT (display), "monitors");
  iterate_for (300);
  g_assert_cmpint (gtk_widget_get_width (guest), ==, 700);

  gtk_window_fullscreen (popup);
  iterate_for (300);
  g_assert_true (gtk_window_is_fullscreen (popup));
  assert_display_corner (guest, FALSE);
  monitor->width = 800;
  monitor->height = 600;
  g_object_notify (G_OBJECT (display), "monitors");
  iterate_for (300);
  g_assert_true (gtk_window_is_fullscreen (popup));
  gtk_window_unfullscreen (popup);
  iterate_for (400);
  width = gtk_widget_get_width (guest);
  height = gtk_widget_get_height (guest);
  g_assert_cmpint (ABS (width * 600 - height * 800), <=, 800);
  assert_display_corner (guest, TRUE);
  g_assert_cmpuint (monitor_updates, ==, updates);

  /* Returning with a fit queued must cancel that work before destroying its window. */
  monitor->width = 640;
  monitor->height = 480;
  g_object_notify (G_OBJECT (display), "monitors");
  pirwa_console_return (console);
  adw_window_set_content (ADW_WINDOW (embedded), GTK_WIDGET (console));
  g_signal_emit_by_name (display, "display-primary-destroy");
  iterate_for (300);
}

static void
check_monitor_bootstrap (gboolean activate, gboolean close_pending)
{
  bootstrap = TRUE;
  needs_activation = activate;
  attempts = n_delays = monitor_updates = pointer_positions = 0;
  display_enabled = FALSE;
  g_autoptr (GArray) monitors = NULL;
  g_autoptr (GSettings) preferences = pirwa_preferences_new ();
  g_autoptr (PirwaConsole) console
      = g_object_ref_sink (pirwa_console_new ("test:///default", uuid, "Disabled monitor"));
  bootstrap_stack = GTK_STACK (find_type (GTK_WIDGET (console), GTK_TYPE_STACK));
  GtkWidget *guest = find_type (GTK_WIDGET (console), PIRWA_TYPE_DISPLAY);
  g_assert_nonnull (guest);
  iterate_for (300); /* Wait for the host monitor before selecting an initial mode. */
  g_assert_cmpuint (monitor_updates, ==, 0);
  GtkWindow *window = GTK_WINDOW (adw_window_new ());
  host_window = window;
  gtk_window_set_default_size (window, 800, 600);
  adw_window_set_content (ADW_WINDOW (window), GTK_WIDGET (console));
  gtk_window_present (window);
  if (close_pending)
    {
      g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (console)));
      gtk_window_destroy (window);
      g_clear_object (&console);
      iterate_for (700);
      g_assert_cmpuint (pointer_positions, ==, 0);
      g_assert_cmpuint (attempts, ==, 1);
      goto cleanup;
    }
  wait_for_display (bootstrap_stack);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
  GdkMonitor *host_monitor
      = gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface);
  g_assert_nonnull (host_monitor);
  GdkRectangle bounds;
  gdk_monitor_get_geometry (host_monitor, &bounds);
  double scale = gdk_monitor_get_scale (host_monitor);
  g_assert_cmpint (requested_width, ==, (int)(bounds.width * scale + 0.5));
  g_assert_cmpint (requested_height, ==, (int)(bounds.height * scale + 0.5));
  g_assert_cmpuint (pointer_positions, ==, activate ? 2 : 0);
  g_assert_cmpuint (attempts, ==, 1);
  g_assert_cmpuint (n_delays, ==, 0);
  g_assert_cmpuint (monitor_updates, >=, 1);
  guint updates = monitor_updates;
  g_signal_emit_by_name (main_channel, "main-agent-update");
  iterate_for (300);
  g_assert_cmpuint (monitor_updates, ==, updates);
  gtk_window_set_default_size (window, 960, 540);
  iterate_for (400);
  g_assert_cmpuint (monitor_updates, ==, updates);
  /* The guest can choose a different aspect ratio without being resized back. */
  g_object_get (display, "monitors", &monitors, NULL);
  g_array_set_size (monitors, 1);
  g_array_index (monitors, SpiceDisplayMonitorConfig, 0)
      = (SpiceDisplayMonitorConfig){ .width = 3, .height = 2 };
  g_object_notify (G_OBJECT (display), "monitors");
  g_assert_cmpint (visible_frame.width, ==, 3);
  g_assert_cmpint (visible_frame.height, ==, 2);
  g_assert_cmpint (visible_frame.stride, ==, 16);
  g_array_index (monitors, SpiceDisplayMonitorConfig, 0).width = 10;
  g_object_notify (G_OBJECT (display), "monitors");
  g_assert_cmpint (visible_frame.width, ==, 4);
  g_assert_cmpint (visible_frame.height, ==, 4);
  g_assert_cmpuint (monitor_updates, ==, updates);
  g_assert_cmpuint (pointer_positions, ==, activate ? 2 : 0);
  g_assert_true (g_settings_set_boolean (preferences, "auto-resize-display", TRUE));
  iterate_for (400);
  g_assert_cmpuint (monitor_updates, >, updates);
  g_assert_cmpint (requested_width, ==, gtk_widget_get_width (GTK_WIDGET (bootstrap_stack)) - 12);
  g_assert_cmpint (requested_height, ==, gtk_widget_get_height (GTK_WIDGET (bootstrap_stack)) - 12);
  g_assert_true (g_settings_set_boolean (preferences, "auto-resize-display", FALSE));
  updates = monitor_updates;
  gtk_window_set_default_size (window, 800, 600);
  iterate_for (400);
  g_assert_cmpuint (monitor_updates, ==, updates);
  check_reactivation (console, window);
  if (!activate)
    check_popup_fit (console, window);
  gtk_window_destroy (window);
  g_clear_object (&console);
cleanup:
  host_window = NULL;
  g_clear_object (&display);
  g_clear_object (&main_channel);
  g_clear_object (&inputs_channel);
  bootstrap_stack = NULL;
  bootstrap = FALSE;
  iterate_for (300); /* Pending resize callbacks must not access the disposed console. */
}

int
main (void)
{
  g_setenv ("GTK_A11Y", "test", TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  adw_init ();
  g_autoptr (PirwaConsole) console
      = g_object_ref_sink (pirwa_console_new ("test:///default", uuid, "Automatic connection"));
  GtkStack *stack = GTK_STACK (find_type (GTK_WIDGET (console), GTK_TYPE_STACK));
  AdwStatusPage *status = ADW_STATUS_PAGE (find_type (GTK_WIDGET (console), ADW_TYPE_STATUS_PAGE));
  g_assert_nonnull (stack);
  g_assert_nonnull (status);
  g_assert_true (GTK_IS_SPINNER (adw_status_page_get_child (status)));
  g_assert_true (gtk_spinner_get_spinning (GTK_SPINNER (adw_status_page_get_child (status))));
  g_assert_cmpuint (attempts, ==, 1); /* Construction starts the connection without input. */
  wait_for_display (stack);
  g_assert_cmpuint (attempts, ==, 7);
  g_assert_cmpuint (socket_errors, ==, 1);
  const guint expected[] = { 1, 2, 4, 8, 16, 16 };
  g_assert_cmpuint (n_delays, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < n_delays; i++)
    g_assert_cmpuint (retry_delays[i], ==, expected[i]);
  iterate_for (300); /* A successful first frame cancels the connection watchdog. */
  g_assert_cmpuint (attempts, ==, 7);

  g_signal_emit_by_name (display, "channel-event", SPICE_CHANNEL_ERROR_IO);
  wait_for_display (stack);
  g_assert_cmpuint (attempts, ==, 8);
  g_assert_cmpuint (retry_delays[n_delays - 1], ==, 1); /* Success resets backoff. */

  g_signal_emit_by_name (display, "channel-event", SPICE_CHANNEL_CLOSED);
  inventory_state (5);
  g_assert_cmpstr (adw_status_page_get_title (status), ==, "Machine is stopped");
  g_assert_false (gtk_widget_get_visible (adw_status_page_get_child (status)));
  iterate_for (300);
  g_assert_cmpuint (attempts, ==, 8); /* Stopping cancels an already queued retry. */
  inventory_state (1);
  wait_for_display (stack);
  g_assert_cmpuint (attempts, ==, 9);

  /* Losing an established surface also needs a watchdog, even without a
   * usable input channel to wake the guest. Previously it stayed empty. */
  g_signal_emit_by_name (display, "display-primary-destroy");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (stack), ==, "status");
  wait_for_display (stack);
  g_assert_cmpuint (attempts, ==, 10);
  g_assert_cmpuint (retry_delays[n_delays - 1], ==, 1);

  /* Dispose with a retry pending, then again with the first-frame watchdog pending. */
  fail_connections = TRUE;
  g_signal_emit_by_name (display, "channel-event", SPICE_CHANNEL_CLOSED);
  g_clear_object (&display);
  g_clear_object (&console);
  iterate_for (300);
  g_assert_cmpuint (attempts, ==, 10);
  fail_connections = FALSE;
  attempts = 1; /* The next fixture intentionally stalls at attempt two. */
  console = g_object_ref_sink (pirwa_console_new ("test:///default", uuid, "Closing connection"));
  g_assert_cmpuint (attempts, ==, 2);
  g_clear_object (&console);
  iterate_for (300);
  g_assert_cmpuint (attempts, ==, 2);
  check_monitor_bootstrap (FALSE, FALSE);
  check_monitor_bootstrap (TRUE, FALSE);
  check_monitor_bootstrap (TRUE, TRUE);
  g_print ("PASS: automatic connection, socket/start/channel errors, first-frame timeout, "
           "bounded backoff, monitor bootstrap and activation before the first frame, recovery, "
           "reactivation pixels, repeated surface loss and wake-up, popup display fitting, "
           "lifecycle and "
           "disposal\n");
  return 0;
}
