# Pirwa

A native GNOME virtual machine manager built around libvirt. Performance and
responsiveness come first. Inspired by GNOME Boxes, built with C17, GTK 4,
libadwaita, libvirt/libvirt-glib, libxml2, libosinfo, and Meson.

**Status: functional v0.1 development build.** Release acceptance remains open
for full Linux installation, real Wayland console/accessibility testing, and
the complete performance contract.

## Features

- Persistent session/system connections, lifecycle events, bounded reconnect,
  search, stable VM identities, and virtualized rows.
- A one-time GNOME Boxes guide when an installed Boxes library contains machines;
  reuse the shared Session library or connect to the Boxes Flatpak library.
  Existing VM definitions and disks stay in place. The Import header action
  reopens the guide later.
- Start, shutdown, restart, pause/resume, and confirmed force stop.
- Local bootable ISO creation and standalone qcow2 import, with progress,
  cancellation, allocation rollback, and source-image preservation.
- Capability-checked KVM/host CPU, virtio devices, UEFI or legacy BIOS,
  userspace networking, SPICE graphics/audio, and OS identification.
- Console via a separate virt-viewer window.
- Rename, CPU/RAM edits, and ISO eject while powered off.
- Confirmed removal; optional disk deletion requires an ownership record,
  matching domain/volume metadata, and checks for references from other VMs.
- Desktop/AppStream integration, gettext infrastructure, Fedora RPM recipe,
  headless tests, native desktop QA, sanitizer checks, and CI configuration.

## Build and run

Fedora development dependencies:

    sudo dnf install gcc meson ninja-build pkgconf-pkg-config gettext \
      glib2-devel gtk4-devel libadwaita-devel libvirt-devel libvirt-glib-devel \
      libxml2-devel libosinfo-devel clang-tools-extra desktop-file-utils appstream

Runtime dependencies for real VMs include the distribution's QEMU/KVM libvirt
stack, osinfo-db, and virt-viewer. Configure the appropriate libvirt daemon
sockets for your distribution.

    sudo dnf install virt-viewer qemu-kvm libvirt-daemon-driver-qemu osinfo-db
    meson setup build
    meson compile -C build
    ./build/src/pirwa

The default is qemu:///session. Choose System explicitly in the header to see
system VMs. Run as your regular desktop user.

For a safe synthetic demonstration without KVM:

    ./examples/run-test-driver.sh

The test driver supports controls and resource edits. QEMU provisioning and a
real graphical console require the real QEMU driver.

## Check

    meson test -C build --print-errorlogs
    clang-format --dry-run --Werror src/*.[ch] tests/*.c
    ./scripts/benchmark.sh

The default suite never changes real guests. An opt-in KVM test creates a
uniquely named disposable pool/VM, checks create/import/edit/rollback/removal,
and cleans its resources:

    PIRWA_RUN_KVM_TESTS=1 ./build/tests/test-kvm

It uses the Fedora UEFI shell ISO by default. Set PIRWA_TEST_ISO to another local
bootable x86 ISO if needed. It does not automate a full Linux installation.

On a desktop, exercise the actual window and test-driver controls:

    ./build/tests/test-ui --exercise
    ./build/tests/test-ui --exercise --large-text --dark --width 720

For an isolated virtual GNOME desktop with a 1280×800, 60 Hz monitor:

    ./scripts/test-gnome.py --output build/qa/gnome

This requires GNOME Shell with headless support and dbus-run-session. It runs
the light, large-text/dark, empty-collection, and Boxes onboarding cases, saves screenshots/logs,
and tears down its private compositor and settings directories.

The desktop tool also measures startup, idle CPU/PSS, and scrolling. See the
[performance workflow](docs/design/performance.md) for commands and measurement limits.

## Documents

- [User guide](docs/user/getting-started.md)
- [Architecture](docs/design/architecture.md)
- [v0.1 plan and remaining gates](docs/design/v0.1-plan.md)
- [Performance contract](docs/design/performance.md)
- [Implementation validation](docs/research/v0.1-validation.md)
- [Console decision](docs/design/console-decision.md)
- [Setup and packaging](docs/implementation/setup.md)

The initial development host uses locally extracted development headers; see the
setup notes before making a fresh build there. Normal package installation is
recommended.

GPL-3.0-or-later. Pirwa is independent of GNOME Boxes. Before public distribution,
choose a controlled application ID and publish a homepage; the current
io.github.pirwa.Pirwa identity is provisional.
