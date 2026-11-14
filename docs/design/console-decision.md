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

Popping out copies one immutable guest frame for the content panel preview.
The preview and top-center return button replace the inline viewer while it is
detached. There is no preview update timer or second display connection.
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

## Interaction

Click the guest to send keyboard, buttons, pointer and vertical wheel input.
Ctrl+Alt or Release Input releases pressed keys/buttons and restores host shortcuts.
Focus loss and disconnection also release input. Standard GNOME Linux evdev/XKB
keycodes are translated to XT set 1. The console has no bottom status bar.
Fullscreen is a header button; the header hides in fullscreen and overlays the
display when the pointer reaches the top edge or Ctrl+Alt releases input. It
hides again when the pointer leaves the header. Overlaying the controls does
not change the guest display allocation. Agent-enabled
guests receive debounced display-size requests; other guests are aspect-fitted.
The first display and cursor channel (ID 0) are supported. Playback uses SPICE's
audio helper. Microphone, USB, smartcard and file channels are not connected.

Lifecycle events reconnect a stopped/restarted guest. Unexpected main-channel
disconnects have three retry attempts, followed by the explicit Reconnect action.
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
