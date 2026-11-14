# Native SPICE console

Pirwa now uses a GTK 4/libadwaita window and `spice-client-glib-2.0` for SPICE
consoles, including existing Boxes machines. It does not embed the GTK 3 SPICE
widget or change a machine's graphics configuration. VNC and machines without a
SPICE device, and SPICE configurations with GL enabled, retain the external
virt-viewer fallback.

## Transport and rendering

Every SPICE channel requests its own authenticated socket using
`virDomainOpenGraphicsFD(..., VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH)` on a libvirt
worker. The UI never blocks on libvirt. FD lists own pending results; cancellation,
window closure, and session generations prevent leaking or attaching stale sockets.
Each console keeps its own backend so it survives closing the collection window.
Repeated Open Console requests present the existing window for that URI/UUID.

SPICE primary surface changes and damage signals drive GTK snapshots. Invalidations
coalesce into the next render; no continuous frame timer or collection screenshot
polling is introduced. Textures own immutable copies because SPICE mutates its
framebuffer. GTK 4.16 memory texture builders receive the previous texture and
damage region to permit partial GPU uploads. CPU snapshots currently copy the
whole primary surface once per rendered update. This is not a zero-copy path.
RGB32, ARGB32, RGB555 and RGB565 surfaces are bounded to 8192 × 8192 pixels.

## Interaction

Click the guest to send keyboard, buttons, pointer and vertical wheel input.
Ctrl+Alt or Release Input releases pressed keys/buttons and restores host shortcuts.
Focus loss and disconnection also release input. Standard GNOME Linux evdev/XKB
keycodes are translated to XT set 1. Fullscreen is a header button. Agent-enabled
guests receive debounced display-size requests; other guests are aspect-fitted.
The first display and cursor channel (ID 0) are supported. Playback uses SPICE's
audio helper. Microphone, USB, smartcard and file channels are not connected.

Lifecycle events reconnect a stopped/restarted guest. Unexpected main-channel
disconnects have three retry attempts, followed by the explicit Reconnect action.
Closing a console closes its sockets and stops its backend, while the VM keeps running.

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
