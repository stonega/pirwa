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
URI/UUID argument shape. Real-KVM tests validate generated SPICE configuration
and guest startup, not an interactive graphical session. Install virt-viewer
for actual desktop use. Input capture/release, fullscreen, resize, audio,
Wayland behavior, and a full Linux installation still need release acceptance.
