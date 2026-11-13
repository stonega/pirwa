# GNOME Boxes onboarding validation

Date: 2026-11-13. This adds access to existing libraries, with no data migration
or disk copying. Native Boxes and Pirwa share persistent Session domains. The
guide does not grant disk ownership, redefine machines, or change their devices.

## Automated coverage

The ninth default Meson check covers native/empty discovery, read-only Flatpak
configuration scanning, duplicate UUIDs, rejected DTDs/symlinks/FIFOs, one-time
preferences, offline socket-name restoration, cancellation, owned task inputs,
and main-thread completion. It uses libvirt's test driver and temporary files.

The virtual GNOME matrix runs these five cases:

- The existing light, 200% dark/narrow, and empty-inventory workflows.
- Boxes onboarding at normal size: automatic display, native library listing,
  offline Flatpak guidance, refresh, Not Now, a second window with no repeat
  prompt, manual reopening, and using an existing VM without adding a duplicate.
- The same onboarding at 200% text/dark/720 px, using the main window close
  request to dismiss the modal and verify that the backend remains usable.

Reproduce with `./scripts/test-gnome.py --output build/qa/boxes-import` after
building. The production window and guide are shared with the QA executable.
The fixtures install a fake Boxes executable for detection; it is never run.
There are no real guests or source disk images in these automated cases.

The default checks and virtual matrix also pass with address/undefined
sanitizers. Leak detection remains disabled for the dependency findings already
documented in setup.md. The UI tests use GTK's test accessibility backend,
not physical input or a screen reader.

Evidence: [normal report](report.json), [sanitizer report](sanitized-report.json),
[onboarding screenshot](guide.png), and [200% dark screenshot](guide-large-dark.png).
Reports mark the feature's then-uncommitted source; the committed feature
contains the validated production code and accompanying review documentation.

## Native library preview

The same production window/guide was rendered inside virtual GNOME against
the host's actual per-user session daemon. Its private test runtime explicitly
linked the existing user's libvirt socket directory. This was a read-only UI
preview: the tool made no lifecycle, configuration, or storage mutation requests.
Temporary XDG state kept the user's first-run preference untouched.

It found boxes-unknown and boxes-unknown-3. Their inactive definition hashes
and stopped power states matched before and after the preview. Evidence:
[native screenshot](native-guide.png), [session report](native-report.json),
and [unchanged VM record](native-unchanged.json).

The stable Boxes Flatpak branch has fixture coverage for its configuration and
socket locations, offline instructions, and preference handling. There was no
installed Boxes Flatpak on this host, so connection/control/viewer behavior
against an actual Flatpak runtime remains an integration acceptance item.

## Source references

Reviewed GNOME Boxes at commit `78cd19e85b70e796aa17ca26fde611e9bd31608a`:
[default Session source](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/data/QEMU_Session),
[collection/guestfs filtering](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/libvirt-broker.vala),
and [Flatpak manifest](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/build-aux/flatpak/org.gnome.BoxesDevel.json).
The host/sandbox directory model follows
[Flatpak's filesystem documentation](https://docs.flatpak.org/en/latest/sandbox-permissions.html).
