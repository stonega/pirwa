# Pirwa

A native GNOME virtual machine manager built around **libvirt**, with performance
as the first priority. Inspired by [GNOME Boxes](https://github.com/GNOME/gnome-boxes).

**Status: project initialized; v0.1 is planned, not complete.** The runnable
scaffold lists VM names and states through a read-only connection. It includes
asynchronous discovery, a virtualized list, refresh, and loading/empty/error
states. Creation, power controls, and console access are upcoming milestones.

## Stack

C17 · GTK 4 · libadwaita · libvirt C API · Meson/Ninja. No application server or
JavaScript runtime. GTK runs on the main thread; libvirt discovery runs on a
worker with one bulk state query.

## Build and run

On Fedora, install development dependencies:

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config glib2-devel \
  gtk4-devel libadwaita-devel libvirt-devel clang-tools-extra desktop-file-utils
meson setup build
meson compile -C build
```

Run with libvirt's built-in test driver; no KVM, guest, or daemon is required:

```sh
./examples/run-test-driver.sh
```

For real VMs, install the distribution's QEMU/KVM libvirt stack, then run:

```sh
./build/src/pirwa
```

The default connection is `qemu:///session`. To inspect system VMs explicitly:

```sh
PIRWA_LIBVIRT_URI=qemu:///system ./build/src/pirwa
```

Session and system inventories are separate. Refresh is manual in this initial
scaffold. Discovering a VM does not change its configuration. The app must run as
your regular desktop user.

## Check

```sh
meson test -C build --print-errorlogs
clang-format --dry-run --Werror src/*.[ch] tests/*.c
./build/src/pirwa --version
./scripts/benchmark.sh
```

Tests cover discovery with the real libvirt test driver, callback delivery on the
caller thread, cancellation, and connection failure. They do not start real VMs.
Install desktop integration with `meson install -C build` after choosing a prefix.

## Project documents

- [First-version plan](docs/design/v0.1-plan.md): scope, milestones, and release gates.
- [Architecture](docs/design/architecture.md): thread boundaries and dependencies.
- [Performance](docs/design/performance.md): provisional budgets and measurements.
- [Setup](docs/implementation/setup.md): dependencies and development workflow.
- [Boxes reference review](docs/research/gnome-boxes.md): lessons and console risk.
- [User guide](docs/user/getting-started.md): current behavior.

GPL-3.0-or-later. Pirwa is an independent project. Its development application ID
is `io.github.pirwa.Pirwa`; choose a controlled reverse-DNS ID before publishing.
