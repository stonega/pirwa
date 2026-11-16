# Fedora COSMIC 42 boot compatibility

Validated on 2026-11-15 with `Fedora-COSMIC-Live-42-1.1.x86_64.iso`.
Local ISO SHA-256:
`c462115f675fb03da5033f697d92d1ddee6396d213e03a94773e84f067b87f00`.
This identifies the tested file; it is not an upstream checksum attestation.

## Cause and change

The reported machine booted Linux, but `cosmic-comp` exited with code 101 and
COSMIC repeatedly restarted its session. Pirwa had created a virtio video device
without 3D acceleration, paired with non-GL SPICE. This matches the upstream
[COSMIC virtio graphics issue](https://github.com/pop-os/cosmic-comp/issues/136).
Upstream also identifies QXL or accelerated virtio as the usable VM configurations
in its [VM setup report](https://github.com/pop-os/cosmic-epoch/issues/1351).

New machines now use QXL with the existing local SPICE transport. Storage and
networking retain virtio. QXL support is checked alongside CPU/SPICE capabilities
before allocating storage. This covers ISO creation and standalone qcow2 imports;
existing definitions and Boxes imports retain their graphics configuration.
No dependency or accelerated scanout path was added. See libvirt's
[video and graphics configuration](https://libvirt.org/formatdomain.html).

## Verification

- The build, all 14 headless Meson checks, formatting, and `git diff --check`
  passed. The configuration regression covers both creation paths, non-GL SPICE,
  virtio storage/networking, and preservation of video during resource edits.
- The opt-in disposable KVM integration executable passed real creation,
  rollback, lifecycle operations, resource edits, ISO eject, protected removal,
  qcow2 import, and backing-chain rejection.
- The production backend created a disposable guest from the exact reported ISO
  with QXL, UEFI, four vCPUs, 2048 MiB RAM, and a new private-pool disk. It reached
  the COSMIC desktop with the Install to Hard Drive icon, panel, and dock.
  The desktop evidence was captured before diagnostic guest service changes.
- The production native console received a 1024×768 SPICE primary surface in
  an isolated GNOME desktop. This checks first-frame transport, not full input,
  audio, accessibility, or installer acceptance. A protected GNOME screenshot
  request failed in the first diagnostic controller; the successful rerun used
  libvirt to capture guest output instead.
- The user-authorized repair of the original machine also reached the COSMIC
  desktop. It shut down through ACPI, retained its two vCPUs, 2048 MiB RAM, disk,
  and ISO, and restarted with QXL. A semantic XML comparison confirmed that only
  the video model changed. Its original definition was backed up before repair.
  Guest startup was slow and initially showed only the desktop background; the
  final check confirmed the panel, dock, and installer icon. No resource increase
  or guest-service workaround was applied to this machine.

Raw manifests, domain XML, native-viewer results, screenshots, and the original
definition backup are retained locally under `build/qa/cosmic-boot/`. The
disposable guest, its disk, and its storage pool were removed. Unrelated existing
domain definitions and states were compared with the recorded baseline.

The old ISO also emitted SPICE guest-agent UID-mismatch errors, and the agent
did not connect in the disposable viewer check. QXL still reached the desktop;
automatic guest resolution adjustment is not verified for this ISO. No guest OS
workaround is part of the application change. No performance improvement or
complete distribution support matrix is claimed.
