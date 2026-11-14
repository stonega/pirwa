# Direct import from Boxes Flatpak

Date: 2026-11-14.

The import dialog now lists only stable Boxes Flatpak machines. Native Boxes
machines are ordinary Session inventory and need no import. First-run detection
checks the installed Flatpak ref, including user and system installations; native
installation and leftover app data do not trigger the prompt.

Import requires a persistent, fully stopped guest with one standalone qcow2 disk
and user networking. It copies the disk and UEFI loader/NVRAM, preserves guest
hardware and OS metadata, creates new UUID/name/MAC values and registers an
independent stopped Session guest. Installation media is ejected. Original
configuration, disk and firmware files remain untouched. The dialog explains
shutdown, storage space, cancellation, source retention and unsupported layouts.

## Validation

The headless `boxes` suite covers installation detection (including residual data
without an installed app), Flatpak-only discovery, XML/file safeguards,
one-time state, cancellation and worker/main-thread delivery.

The `boxes-import` suite uses libvirt's test driver, real temporary files and a
small emulation of the driver's missing upload transport and shared connection
state. It covers independent copied disk/firmware bytes, ownership, retained
CPU/memory/OS metadata, source XML/power-state preservation, rejection of running
and unsupported machines, cancellation cleanup, concurrent source replacement,
upload failure, internal snapshots, and firmware no-overwrite/symlink protection.
The new discovery/import suites also pass address/undefined sanitizer checks
with dependency leak detection disabled, following the existing project setup.

The isolated GNOME matrix covers ordinary UI flows and the Flatpak dialog in
normal and 200% dark/narrow layouts. It checks offline guidance and refresh,
Flatpak-only rows, dismissal and reopening across windows, import connection
errors with usable retry controls, and first-run suppression after uninstalling
with residual data still present. All fixtures are synthetic; the socket used for
error handling cannot connect to a daemon. Evidence is under the ignored
`build/qa/boxes-import/` directory (report, screenshots and logs).

No Boxes Flatpak is installed on the implementation host. Real Flatpak daemon
compatibility, copied firmware boot, source/host QEMU version compatibility and
real guest boot remain integration acceptance items. No real user guest was
migrated to validate this change.

## References

- [Flatpak info](https://docs.flatpak.org/en/latest/flatpak-command-reference.html#flatpak-info): default user/system lookup and installed deployment locations.
- [Libvirt offline migration](https://www.libvirt.org/migration.html#offline-migration): definition transfer does not copy non-shared storage or UEFI variable files.
- [Libvirt firmware selection](https://www.libvirt.org/kbase/secureboot.html): explicit loader/NVRAM state and host firmware selection behavior.
