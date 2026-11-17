# Native SPICE console

Pirwa uses a reusable GTK 4 widget and `spice-client-glib-2.0` for SPICE
consoles, including existing Boxes machines. It does not embed the GTK 3 SPICE
widget or change a machine's graphics configuration. VNC and machines without a
SPICE device, and SPICE configurations with GL enabled, retain the external
virt-viewer fallback.

## Transport and rendering

Every SPICE channel requests its own authenticated socket using
`virDomainOpenGraphicsFD(..., VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH)` on a libvirt
worker. The UI never blocks on libvirt. FD lists own pending results; cancellation,
window closure, and session generations prevent leaking or attaching stale sockets.
Each console keeps its own backend. The selected running machine opens inline;
an explicit pop-out reparents the same widget into a libadwaita window without
reconnecting. Returning reparents it back. The collection holds a reference only
to the selected session; detached windows keep their own sessions alive. Selection
and connection changes dispose the previous inline session and retain explicit
popups. Detached lookup uses both connection URI and domain UUID.

Detached windows fit the visible guest monitor plus the measured native header,
using the initial guest mode until a frame arrives. Compositor bounds limit the
size while preserving the guest aspect ratio. Each dimension is capped at 88%
of those bounds to leave desktop space and avoid GNOME automatically maximizing
near-screen-sized windows. Guest monitor size changes refit
an ordinary popup; unchanged geometry leaves manual window sizing alone.
Maximized and fullscreen windows keep their state and refit on restoration.
When automatic guest resolution adjustment is enabled, subsequent guest size
updates do not resize the popup, avoiding a viewport/guest feedback loop.
Sizing follows display and window events, with one cancellable idle callback
to coalesce changes; it introduces no polling or new dependency.

Popping out copies one immutable guest frame for the content panel preview.
The preview replaces the inline viewer while it is detached. A centered message
with a return button directly below it overlays the preview. There is no preview
update timer or second display connection.
Closing a popup returns it if its machine is still selected in an open collection;
otherwise it closes the session. Closing the collection preserves explicit popups.
Input is released before each reparent, and shortcut inhibition follows the
widget's current root window.

SPICE primary surface changes and damage signals drive GTK snapshots. Invalidations
coalesce into the next render; no continuous frame timer or collection screenshot
polling is introduced. Textures own immutable copies because SPICE mutates its
framebuffer. GTK 4.16 memory texture builders receive the previous texture and
damage region to describe changes to GTK; this does not guarantee partial GPU
uploads. `pirwa-display.c` retains a base texture and bounded replacement tiles
for small opaque RGB changes. Dense/ARGB updates, Cairo and minified sources
with incompatible mipmap dimensions use full snapshots. This is not a zero-copy
path. See the [renderer performance record](../research/display-performance/README.md).
RGB32, ARGB32, RGB555 and RGB565 surfaces are bounded to 8192 × 8192 pixels.
When the host window becomes active, resumes from compositor suspension, or the
console maps again, the viewer invalidates its whole visible framebuffer. This
copies the current SPICE pixels and rebuilds the cached GTK rendering even when
an idle guest sends no new damage on return. Healthy sessions keep their existing
transport and guest resolution; recovery adds no polling.

## Interaction

Click the guest to send keyboard, buttons, pointer and vertical wheel input.
Ctrl+Alt or Release Input releases pressed keys/buttons and restores host shortcuts.
Focus loss and disconnection also release input. Standard GNOME Linux evdev/XKB
keycodes are translated to XT set 1. The console has no bottom status bar.
Fullscreen is a header button; the header hides in fullscreen and overlays the
display when the pointer reaches the top edge or Ctrl+Alt releases input. It
hides again when the pointer leaves the header. Overlaying the controls preserves
the guest display allocation. The initial guest mode matches the pixel resolution
of the monitor containing the host window, including its display scale. Each axis
is bounded to the renderer's 8192-pixel limit. Selection waits for the console to
be mapped, then the mode is cached until the SPICE agent is available.
Window resizing, fullscreen and pop-out/return
scale the image while preserving its aspect ratio. Later changes in the guest's
Display settings are respected, including during transport reconnects within the
same console. A newly opened console starts with the default mode again.
Preferences offers “Automatically adjust guest resolution”, disabled by default.
Enabling it restores debounced viewport-size requests for all open consoles,
including detached viewers. Disabling it keeps the current guest mode and cancels
pending resize work. The boolean is persisted with GSettings; this uses the
existing GLib/GIO dependency. Installed and development builds load the same
schema, and test desktops use an isolated memory settings backend.
Guests without an agent keep their existing mode and are aspect-fitted.
The unused viewer area shows the same native surface as the header in light and
dark themes. The embedded viewer has 6 px insets matching the content header
controls. The fitted guest desktop has 12 px rounded corners in inline and
windowed pop-out viewers. Fullscreen removes the guest corner rounding; leaving
fullscreen or returning to the collection restores it.
SPICE monitor 0's geometry determines the visible area, rather than the QXL
allocation size. Rendering, damage coordinates, pointer mapping and the detached
preview use that same area, excluding alignment padding and stale video memory.
Missing or out-of-bounds monitor geometry falls back to the validated primary
surface until a valid configuration arrives. A single opaque texture that fills
the widget uses a direct rounded clip. Layered updates, ARGB and letterboxed views composite through a
luminance mask so corner coverage is applied once. The
[rounding and memory investigation](../research/renderer-memory/README.md)
records the guarded optimization and pixel comparisons.
The first display and cursor channel (ID 0) are supported. Playback uses SPICE's
audio helper. Microphone, USB, smartcard and file channels are not connected.

Selecting a running guest starts its connection automatically. Before the first
frame, Pirwa enables monitor 0 and supplies its initial mode once the host monitor
is known, independently of the guest widget's allocation. With automatic
adjustment off, configuration is sent once per
transport connection and cached until the guest agent is available. This lets a disabled guest monitor produce
its first frame automatically without tying guest resolution to window resizing.
If the console is mapped and its input channel is ready but no primary surface
arrives within 500 ms, it sends two adjacent pointer positions near the guest
monitor's center. This one-pixel movement wakes idle QXL displays without clicks
or keys.
It runs once per missing surface; a primary surface or disconnection cancels the
pending activation. Only mapped consoles in an active, non-suspended host window
send this activation. Losing an established primary surface shows connection
status, permits another activation, and rearms the 15-second frame timeout.
Returning to the foreground lets a viewer with a missing surface try this wake-up.
Lifecycle events reconnect a stopped/restarted guest. Socket failures and main/display-channel
disconnects retry automatically after 1/2/4/8/16 seconds, capped at 16 seconds
while the guest is running. A 15-second first-frame timeout also schedules a
retry, so an initiated connection cannot leave the viewer waiting indefinitely.
A spinner accompanies connection status; no Reconnect button is needed.
Connection and stopped status show the machine's chosen icon, matching the
sidebar's expression and running-state screen fill. Changes saved in Settings
also update inline and detached consoles without reconnecting.
The first valid frame resets backoff and cancels the timeout. Stopping the guest
or disposing its console cancels pending connection and retry timers.
Disposing a console closes its sockets and stops its backend, while the VM keeps running.

The [native console validation](../research/native-console/README.md) records the
virtual GNOME input, resize, playback and reconnect checks.

## Dependencies and scope

Required: GTK ≥ 4.16 and spice-client-glib ≥ 0.42. The development host has
GTK 4.24.1 and SPICE GLib 0.43. No GTK 3 library is loaded by the native console.
The original external-viewer decision remains in Git history; the user's request
for a native console supersedes that fallback for SPICE.

Multiple monitors, accelerated GL/DMABUF scanout, pointer locking for games,
clipboard, microphone and physical input/audio acceptance remain follow-up work.
SPICE GL configurations use the external viewer until native accelerated scanout
is implemented; no automatic device changes occur.

References: [SPICE socket sessions](https://www.spice-space.org/api/spice-gtk/SpiceSession.html),
[SPICE channels](https://www.spice-space.org/api/spice-gtk/SpiceChannel.html),
[SPICE audio](https://www.spice-space.org/api/spice-gtk/SpiceAudio.html),
[GTK immutable objects](https://docs.gtk.org/gtk4/section-threading.html).
