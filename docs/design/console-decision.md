# v0.1 console decision

Use external virt-viewer for v0.1. Keep GTK 4/libadwaita in the manager and use
libvirt's URI/UUID to attach the viewer to SPICE or VNC domains.

Libmks is not available on the implementation host. Its reviewed API was alpha,
and this session cannot establish the required input/audio/Wayland performance
matrix for an embedded implementation. Therefore the native-display gate did
not pass; this uses the fallback explicitly allowed in the original plan.

New guests require capability-checked SPICE graphics and use a local socket,
virtio video, a SPICE guest-agent channel, and SPICE audio. No display is opened
or sampled for collection rows. Open Console starts virt-viewer with attach,
reconnect, and wait options using an argument array. Missing executables and
nonzero viewer exits are reported. Closing Pirwa does not stop a guest/viewer.

The default tests replace virt-viewer with a fixture that verifies the exact
URI/UUID argument shape. The earlier automated real-KVM tests validate generated
SPICE configuration and guest startup. A separate
[Fedora installation run](../research/linux-install/README.md) now covers the
production creation form, graphical installation, shutdown, Settings ISO eject,
disk boot, and reopening the actual viewer in a private GNOME Wayland desktop.
Keyboard and pointer input and fullscreen switching worked there. Its minimal
desktop needed private audio policy services before the normal viewer launch
connected promptly. Install virt-viewer for desktop use.

The [console/performance follow-up](../research/console-performance/README.md)
also confirms guest framebuffer resizing, injected input release/recapture,
same-process viewer reconnect after power cycling, and a guest-generated 440 Hz
tone captured digitally at the host's private audio sink.

Physical audio output/latency, microphone forwarding, clipboard, physical
capture/release, and the broader Wayland/performance matrix remain release
acceptance work.
