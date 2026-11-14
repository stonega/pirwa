# Local development

## Requirements

Meson ≥ 1.3, a C17 compiler, Ninja, pkg-config, GLib/GIO ≥ 2.76, GTK ≥ 4.16,
libadwaita ≥ 1.5, libvirt ≥ 9.0, libvirt-glib ≥ 4.0, libxml2 ≥ 2.9, libosinfo ≥ 1.10, and spice-client-glib ≥ 0.42. Development headers are required, not just
runtime libraries. Build checks use clang-format and optionally desktop-file-validate.

Fedora development packages and normal commands are in the root README. Runtime
QEMU/KVM setup is separate: install the distribution's virtualization packages,
verify hardware virtualization, and use the documented session/system daemon
configuration for that distribution. Do not run the GUI as root or change socket
permissions to bypass authorization.

## Build modes

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
clang-format --dry-run --Werror src/*.[ch] tests/*.c
```

The default is `debugoptimized` with warnings treated as errors. Libvirt headers
are system includes so vendor enum definitions do not fail C17 pedantic checks;
application code retains strict warnings.

For an existing build, use `meson setup build --reconfigure`. After changing
dependency installations or compiler settings, use a fresh build directory.

Once `build` is configured, `./scripts/run-dev.sh` rebuilds and launches Pirwa.
It resolves the project directory automatically, forwards application arguments,
and opens SPICE in the native GTK 4 window. VNC and SPICE GL use an installed virt-viewer
or the temporary development viewer recorded
in `/tmp/pirwa-build-deps-path`. Run it as your regular desktop user.

For a sanitizer run:

```sh
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs
```

For native installation under your account:

```sh
meson setup build --reconfigure --prefix="$HOME/.local"
meson install -C build
```

## Tests and smoke checks

The headless tests use `test:///default`. The test driver ships with typical
libvirt installations but some distributions split it into a separate package.
It requires neither a running libvirt daemon nor KVM. It must remain the default
for automated tests. `PIRWA_RUN_KVM_TESTS=1 ./build/tests/test-kvm` is a separate,
explicitly opt-in workflow using disposable resources; see the README.

Default checks cover inventory, configuration, stable model reconciliation,
backend behavior, storage rollback/reference protection, 1,000-domain scale, and
desktop/AppStream metadata, plus Boxes Flatpak installation detection, discovery,
preferences, import and cancellation.
None requires a display. `gio-unix-2.0` is an explicit GLib component dependency
for desktop-entry detection; Fedora's existing glib2-devel package supplies it.
The usage check covers CPU counter units, idle and multi-core values, network
byte rates, multi-interface totals, missing samples, counter resets, interface
changes, and overflow. Backend tests verify read-only bulk telemetry,
stopped machines, cancellation, and that telemetry does not refresh inventory.

`./build/tests/test-ui --exercise` requires a real desktop display and uses only
the libvirt test driver. It runs the production window, checks lifecycle actions,
settings, cancellation of the destructive dialogs, row-menu selection and UUID
targeting, accessible header labels,
search focus, stable selection, creation-button visibility, and closing with a
detail request pending. Add `--large-text --dark --width 720` for the narrow,
200% font variant. This is a programmed action test, not physical input or a
screen-reader certification. `--snapshot FILE.png` captures only Pirwa's window.
This executable is deliberately excluded from the headless Meson test command.

`./build/tests/test-console-ui --output build/qa/viewer` requires a desktop and
uses two synthetic SPICE definitions with the libvirt test driver. It checks
automatic embedding, title-bar controls, pop-out/return identity, fullscreen,
selection changes, popup closure, and collection closure with a surviving popup.
It also checks that host information and selected VM usage stay in the right
pane's header, with no bottom bar. The header uses one row, with controls on the
left and host information, usage, and state grouped on the right. Hidden or
suspended windows retain their last reading or initial sampling status without polling.
No real SPICE server or guest is needed; display connection errors are expected.
Run it inside the isolated desktop with:

```sh
./scripts/test-gnome.py --output build/qa/viewer-desktop -- \
  ./build/tests/test-console-ui --output build/qa/viewer
```

Add `--dark --large-text --width 720` to exercise the narrow 200% text layout.

`./scripts/test-gnome.py --output build/qa/gnome` runs the seven UI variants in
one disposable GNOME Shell Wayland desktop (1280×800 at 60 Hz). Install
gnome-shell and dbus-run-session first. Runtime, configuration, data, cache,
D-Bus, state, and GSettings are isolated; no settings are written to your desktop.
The wrapper disables GVFS FUSE mounting and unmounts remaining test-session FUSE
exports before removing its directories. The result directory contains report.json, app-only PNG
screenshots, per-case stdout/stderr, the compositor log, and the session log.
Use `--build build-sanitize` with the sanitizer environment to exercise that
binary. An optional command after `--` runs inside the virtual desktop instead
of the default matrix. For a longer interactive installation check, increase
`--command-timeout` (default 600 seconds, maximum 7200). This extends only the
command deadline; it does not provision a VM or add audio services. Custom
commands receive `PIRWA_QA_ISOLATED=1` and must report their own acceptance results.

The matrix includes Boxes Flatpak onboarding and viewer pop-out/return at normal
size and at 200% text/dark/720 px. Its fixtures check Flatpak-only rows, offline
instructions, refresh, dismissal, one-time behavior across windows, manual
reopening, import connection errors and retry, and first-run suppression after
uninstalling despite residual definitions. Native domains remain available without
any import. The synthetic
fixtures do not connect to your real guests.

The headless `boxes-import` test exercises real libvirt test-driver definitions
and storage metadata, with temporary files and a small emulation of the upload
transport that this driver lacks. It checks an independent stopped copy,
configuration/OS preservation, ownership, source immutability, rejection of
running/unsupported guests, cancellation/rollback and firmware file protection.
Actual QEMU boot and a real installed Boxes Flatpak remain integration acceptance
items; the synthetic driver cannot certify guest boot or deployment compatibility.

This is GNOME Shell and its compositor with a virtual monitor, not a complete
logged-in hardware session. Missing authentication/camera/PipeWire services and
the host's AT-SPI activation failure can appear in session logs. The UI exercise
uses GTK's test accessibility backend; it does not certify screen-reader access.
The [Linux installation record](../research/linux-install/README.md) describes
the separate real-guest run, including the private audio services needed by the
viewer. The seven-case matrix above continues to use synthetic VMs only.

The same tool supports profiling without `--exercise`; the Python standard-library
wrapper `scripts/profile-ui.py` creates isolated 0–1,000-domain fixtures and saves
raw measurements. See the performance design for methodology.

`examples/run-test-driver.sh` launches the UI with synthetic domains on your
normal desktop. `PIRWA_LIBVIRT_URI` affects only a newly launched application
process; an already-running single-instance app keeps its connection.

Extended opt-in checks use `scripts/console-session.py`, `scripts/profile-guest.py`,
and the non-default `qa-vm` executable. See the
[console/performance record](../research/console-performance/README.md) for their
private desktop/audio setup, measurements, identity guards, and cleanup.
These tools require `PIRWA_RUN_KVM_TESTS=1` and disposable guests. They add no
runtime dependency to the application. In `qa-vm`, `stop` means force stop and
is intended only for test-resource cleanup.

`scripts/benchmark.sh [build-directory]` runs the headless inventory benchmark.
It uses the built-in test driver and does not touch real guests.

`scripts/profile-console.py` is an opt-in real-guest display profiler. It needs
`genisoimage`, a local session libvirt socket, a Fedora Workstation Live 43 ISO,
and the existing virtual GNOME/Python GI tools. Build `benchmark-console` and
`qa-vm` in release mode first. The runner creates and cleans its own disposable
VM, volume and pool; it is excluded from default tests. Commands and measurement
limits are in the [active console record](../research/active-console-performance/README.md).

## Bootstrap host note

The initialization host had GTK/libadwaita development packages and libvirt
12.6.0 runtime libraries, but no `libvirt-devel`. Passwordless sudo was unavailable.
Validation used the matching Fedora `libvirt-devel-12.6.0-2.fc45.x86_64` RPM
extracted into a task-owned `/tmp/pirwa-build-deps.*` directory. Its local
pkg-config file points to extracted headers and a symlink to the installed
runtime library. No system package installation was performed.

That temporary build environment is not a project dependency or a durable SDK.
Install `libvirt-devel` normally before creating a new build on this host; remove
the old build directory or reconfigure after the temporary files disappear.


## Implementation host and packaging

The implementation also extracts matching libvirt-glib-devel and libosinfo-devel
RPMs into the same temporary prefix. GCC libasan/libubsan runtime RPMs were
extracted to run address/undefined checks without changing system packages.
For those local sanitizer builds, pass the temporary library directory as a
linker search path and LD_LIBRARY_PATH. ASAN_OPTIONS=detect_leaks=0 separates
address/undefined checks from the known dependency leak findings.

Install headers normally on a durable development host. On this host, the
already-configured build directory can be compiled directly while the temporary
prefix exists.

The Fedora RPM recipe is packaging/pirwa.spec. Create a source tarball from the
committed tree with meson dist -C build --no-tests, place its xz archive in the
RPM SOURCES directory, then run rpmbuild -ba packaging/pirwa.spec after installing
its BuildRequires. The native Meson install path also works without RPM.

The development AppStream check overrides only the missing-homepage warning.
A real homepage and controlled application ID are required before publication.

## Native SPICE development headers on this host

The console adds `spice-glib-devel` and `spice-protocol`, with GTK 4.16 required
for memory textures with damage hints. GTK 3 headers are not used. On this host,
the signed matching Fedora `spice-glib-devel-0.43-5.fc45.x86_64` and
`spice-protocol-0.14.5-5.fc45.noarch` RPMs were extracted into the existing temporary
SDK prefix. The local SPICE `.pc` omits `Requires.private` (only the shared library
is linked; its runtime dependencies are installed) and its linker symlink points
to the installed runtime. This is a local SDK workaround, not a repository build
requirement. Normal `dnf install spice-glib-devel` supplies all development dependencies.

For the extracted SDK, both pkg-config directories are needed:

```sh
pirwa_sdk=$(cat /tmp/pirwa-build-deps-path)
meson setup build --reconfigure -Dpkg_config_path="$pirwa_sdk/usr/lib64/pkgconfig,$pirwa_sdk/usr/share/pkgconfig"
```
