# Local development

## Requirements

Meson ≥ 1.3, a C17 compiler, Ninja, pkg-config, GLib/GIO ≥ 2.76, GTK ≥ 4.16,
libadwaita ≥ 1.5, libvirt ≥ 9.0, libvirt-glib ≥ 4.0, libxml2 ≥ 2.9, libosinfo ≥ 1.10,
spice-client-glib ≥ 0.42, and libslirp ≥ 4.7. Development headers are required, not just
runtime libraries. Build checks use clang-format and optionally desktop-file-validate.
RPM builds also require the AppStream CLI for strict staged-metadata validation.

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

`./scripts/run-dev.sh` configures `build` if needed, rebuilds and launches Pirwa.
It resolves the project directory automatically, forwards application arguments,
and opens SPICE in the native GTK 4 window. VNC and SPICE GL use an installed virt-viewer
or the temporary development viewer recorded
in `/tmp/pirwa-build-deps-path`. Run it as your regular desktop user.

### Launcher fails after temporary SDK cleanup

The development launcher rebuilds before opening the window. A configured build
can retain paths to deleted headers, pkg-config files and linker symlinks under
`/tmp`; an existing executable does not make that rebuild succeed. On this host,
that produced missing `libvirt/libvirt.h`, `spice-client.h` and `libslirp.h`
errors before the window could open.

Prefer the normal Fedora development packages listed in the README. After
installing them, discard the cached temporary paths:

```sh
meson setup build --reconfigure --clearcache '-Dpkg_config_path=[]'
./scripts/run-dev.sh
```

The current host's unprivileged workaround keeps development files under the
ignored `.cache/dev-sdk/` directory in this checkout, instead of `/tmp`. To
reconfigure against those existing files:

```sh
pirwa_sdk="$PWD/.cache/dev-sdk/usr"
meson setup build --reconfigure --clearcache \
  -Dpkg_config_path="$pirwa_sdk/lib64/pkgconfig,$pirwa_sdk/share/pkgconfig"
./scripts/run-dev.sh
```

Run those commands from the repository root. The SDK was copied from the
existing rootless Fedora QA image with networking disabled; package versions
and the image ID are in `.cache/dev-sdk/provenance.txt`. Its linker symlinks use
the host runtime libraries, and the local SPICE pkg-config file omits static-only
dependencies. This remains a host workaround; no SDK is downloaded or installed
by the launcher. A fresh checkout needs normal development packages or its own
explicit SDK setup. Use the clean native package check for release build evidence.

For a sanitizer run:

```sh
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp:print_suppressions=1" \
  meson test -C build-asan --print-errorlogs
```

The headless sanitizer check enables leak detection. `tests/lsan.supp` excludes
only the independently reproduced `virDataTypesOnce` initialization allocations
in libvirt 12.6.0. Application allocations remain checked. The
[lifecycle follow-up](../research/lifecycle-leaks/README.md) records the control,
the backend fix and the separate GUI/graphics-driver findings. This host also
needs the extracted sanitizer runtime in `LD_LIBRARY_PATH`, as described below.

For native installation under your account:

```sh
meson setup build --reconfigure --prefix="$HOME/.local"
meson install -C build
```

## Clean native build and package check

Use rootless Podman to validate a frozen source snapshot with Fedora RPM
dependencies, without the extracted development SDK:

```sh
./scripts/check-clean-build.py --build-image --rpm \
  --output build/qa/clean-native-new
```

The image build uses an empty context and installs normal build/runtime RPMs.
If your package proxy listens on the host's loopback interface, add
`--build-network host`. Only dependency installation needs network access;
the compilation, headless checks, sanitizer checks, staging and RPM build/install
run with networking disabled. Package BuildRequires are enforced. The RPM is a
development artifact until release acceptance and metadata are complete.

The runner copies Git-tracked and non-ignored new files into a source snapshot,
then mounts it read-only. No live source, system directory or credential store
is mounted into the check container. Results include the source manifest,
image ID, package versions, logs, test results and RPMs. The copied executables
include their matching compiled GSettings schema; private-daemon tools pin both
and use an in-memory settings backend. Source drift after the snapshot is
reported separately. This is build/install evidence, not hardware-session QA.

## Tests and smoke checks

Machine avatar preferences are covered by the headless `preferences` test,
including connection isolation, reopening settings and resetting overrides.
The explicit desktop check uses only `test:///default` and verifies icon
selection, search, Cancel, persistence across windows, unknown presets,
and icon-only saves while running. A linker wrapper rejects any guest edit
request in this cosmetic-only workflow:

```sh
./scripts/test-gnome.py --output build/qa/avatars-light -- \
  ./build/tests/test-avatar-ui --output build/qa/avatars-light/screenshots
./scripts/test-gnome.py --output build/qa/avatars-dark -- \
  ./build/tests/test-avatar-ui --dark --large-text --width 720 \
  --output build/qa/avatars-dark/screenshots
```

The 16 robot expressions and monitor frame are original Pirwa SVG artwork;
their construction and licensing are recorded in `data/avatars/README.md`.
The build embeds them in a separate generated-resource library, with its
overlength-string warning exception limited to generated C. Application code
retains strict compiler warnings. This adds no runtime dependencies.

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

`backend-lifetime` checks 16 rounds each of connect/reconnect, failed connection
and stopping before opening. Test-only wrappers observe the backend queue's
release and join its worker before checking that the backend has finalized.
It uses real test-driver operations. Production shutdown still does not join
workers on the GTK thread.

`backend-faults` runs seven subprocess cases with the real libvirt test driver
and test-only linker wrappers. It covers failed startup and recovered pool/host
metadata, bounded reconnect delays, manual retry after exhaustion, cancellation
during backoff, close notifications, permission failures, failed power commands
without replay, and shutdown during a blocked inventory call. The blocked call
also verifies that the main loop can dispatch an idle callback. Retry waits are
shortened only in this executable; the assertions inspect the production delay
values. No service is restarted and no real VM is opened. See the
[backend recovery record](../research/backend-recovery/README.md).

```sh
meson test -C build backend-faults --print-errorlogs
```

The opt-in private-daemon check exercises the production backend against a real
`virtqemud` and its QEMU driver. Run it as your regular user, with a fresh output
directory:

```sh
meson compile -C build test-backend-daemon
PIRWA_RUN_DAEMON_TESTS=1 ./scripts/test-libvirt-daemon.py \
  --build build --output build/qa/private-daemon-new
```

It starts an empty session in private runtime/config/data/cache/state directories,
with daemon autostart disabled and an explicit private socket. It tests startup
failure, graceful/crash restart, denied socket permissions and shutdown during
backoff. It defines no VM, storage pool or network, and needs no KVM or display.
Only its own daemon is stopped and its own socket permissions are changed.
SIGTERM cleanup is supported. The executable is excluded from the default test
suite. Use `--build build-sanitize` with the documented sanitizer environment for
the same checks under ASan/UBSan. See the
[live-daemon results](../research/backend-recovery/README.md#private-qemu-daemon-follow-up)
for scope: system polkit and installed systemd units remain separate checks.

Add `--socket-activation` to use the normal `qemu:///session` URI against a
private listener created by `systemd-socket-activate`:

```sh
PIRWA_RUN_DAEMON_TESTS=1 ./scripts/test-libvirt-daemon.py \
  --socket-activation --build build --output build/qa/socket-activation-new
```

This opt-in variant requires `systemd-socket-activate`. It verifies that the
first connection starts the daemon using its pre-opened socket, then exercises
graceful/crash restart, permission denial and shutdown during retry backoff.
The client validates all five private XDG directories before using the default
URI; libvirt client autostart stays disabled. It does not install or operate
systemd units. Both variants remain outside the default headless suite. See the
[activation record](../research/backend-recovery/README.md#private-socket-activation-follow-up)
for cleanup controls and deployment limits.

Running-guest recovery has a separate opt-in Linux fixture:

```sh
PIRWA_RUN_DAEMON_TESTS=1 PIRWA_RUN_KVM_TESTS=1 ./scripts/test-guest-recovery.py \
  --build build --output build/qa/guest-recovery-new
```

It uses an isolated, socket-activated session and a generated UUID. A 512 MiB
Linux guest boots a freestanding QA initramfs, writes synced heartbeat records
to ext4 on a 64 MiB virtio qcow2 disk, and continues across graceful/crash daemon
restart. The recovered backend pauses/resumes it. Clean shutdown is followed by
`qemu-img check`, an offline filesystem check and a second boot reading the
saved record. `--disk-format raw` is an additional storage-path check.

Requirements: x86_64 KVM, GCC, virsh, qemu-img, e2fsprogs and the private-daemon
tools above. The selected `/boot/vmlinuz-VERSION` needs a sibling `config-VERSION`
with built-in initramfs/gzip, PCI, devtmpfs, virtio block/PCI, ext4 and serial
console support; use `--kernel` to select it. No host initrd or root access is
needed. The runner requires the guest's 512 MiB plus 1024 MiB host headroom.
Only its generated guest is operated, and cleanup verifies its exact QEMU
process has exited. It remains outside `meson test`. ASan/UBSan instruments the
Pirwa backend client; it does not instrument the guest kernel or QEMU.

`./build/tests/test-ui --exercise` requires a real desktop display and uses only
the libvirt test driver. It runs the production window, checks lifecycle actions,
settings, cancellation of the destructive dialogs, row-menu selection and UUID
targeting, accessible header labels,
search focus, stable selection, creation-button visibility, and closing with a
detail request pending. Add `--large-text --dark --width 720` for the narrow,
200% font variant. This is a programmed action test, not physical input or a
screen-reader certification. `--snapshot FILE.png` captures only Pirwa's window.
This executable is deliberately excluded from the headless Meson test command.

The row-menu checks include on-demand popup creation, filtering a row out while
holding its button, clearing its old popup, and restoring the original selection
before checking Settings/Force Stop/Remove targets. With a multi-machine test
fixture, filtering to the second machine also verifies every new menu target
before restoring the first. These checks use only synthetic domains.

`test-ui-lifetime` checks actual finalization of Pirwa windows, dialogs, models,
settings and SPICE sessions/channels/audio helpers. It alternates stopped-guest
settings with detached-console closure, including closing the parent while a
dialog is open. A held-reference control distinguishes disposal from finalization.
Settings checks inject a same-VM inventory refresh during its details request
and require the dialog to open; clearing selection during a separate request
must discard the pending opening intent.
Only private test-driver definitions are used. Test-only worker joins allow
asynchronous cleanup to complete; the application still never joins on GTK.
The standard desktop matrix includes eight cycles, or request up to 32 explicitly:

```sh
meson compile -C build test-ui-lifetime
./scripts/test-gnome.py --output build/qa/ui-lifetime-new -- \
  ./build/tests/test-ui-lifetime --cycles 32
```

The target also builds its matching settings schema. The standard desktop matrix
uses the selected build's schema directory, including for exported native-check
binaries, and records the compiled schema's hash. Custom commands retain their
own schema setup. See the
[GUI lifetime investigation](../research/gui-lifetimes/README.md) for normal and
sanitizer results, the independent GTK accent-color leak reproducer and the
remaining Fontconfig reports. Object finalization is separate from full
LeakSanitizer acceptance of toolkit/driver allocations.

The action test also checks that closing search returns focus to the machine
list. Programmed forward/reverse GTK focus traversal stays within the creation
dialog and scrolls its ISO chooser fully into view, including at 200% text size.
All unexpected GTK/GObject warnings remain fatal. See the
[focus navigation validation record](../research/keyboard-navigation/README.md)
for the evidence and the limits of this programmed check.

`test-create-ui` exercises the four creation steps with synthetic file drops.
It checks local-file validation, filename defaults, custom-name retention, bottom
step navigation, configuration, the submitted options, busy-state cancellation and
retry, qcow2 import, and missing storage pools. It submits no backend operation
and creates no guest. It also checks that the step buttons align vertically and
fit their bar, and captures a hovered previous step beside the active highlight.
Run it in the isolated desktop, with screenshots:

```sh
./scripts/test-gnome.py --output build/qa/create-desktop -- \
  ./build/tests/test-create-ui --output build/qa/create
```

Add `--large-text --dark --width 720` to the test executable's arguments to check
the narrow layout at 200% text size. Page content scrolls while navigation stays
visible. This executable is also excluded from the headless suite.

`./build/tests/test-console-ui --output build/qa/viewer` requires a desktop and
uses three synthetic SPICE definitions with the libvirt test driver. It checks
first-machine selection, restoration of the last selected UUID after reopening,
selection preservation across refresh, and fallback when the remembered machine
is missing. The memory settings backend keeps these choices isolated from your
desktop. Headless preference checks cover independent choices per connection.
It also checks
automatic embedding, title-bar controls, pop-out/return identity, fullscreen,
selection changes, popup closure, and collection closure with a surviving popup.
It also checks a centered Start below the stopped machine's resources, no display
hint, and hidden header controls and resources while stopped. For a running machine,
the selected VM's configured resources and usage stay in the right
pane's header, with no bottom bar. The header uses one row, with controls on the
left and compact VM configuration and usage on the right. Distinct host and guest
resource values, selection changes, and clearing selection verify the configuration
source. Running and Powered off state labels stay hidden. Hidden or
suspended windows retain their last reading or initial sampling status without polling.
No real SPICE server or guest is needed; display connection errors are expected.
The media fixture checks an ellipsized ISO filename, full tooltip, eject icon,
placement immediately after Shut Down, visibility across selections, and hiding
after a real test-driver eject. It holds ejection pending to check the button's
circular spinner, unchanged width, hidden global progress row, and recovery
after an injected failure followed by a successful retry.
Headless backend tests cover running, paused and powered-off media removal,
persistence across the next start, stale-source
rejection and cancellation. Both executables verify the force flag on live
ejection, then strip it in a test-only adapter because libvirt's test driver does
not accept it. The test driver does not certify QEMU media locks.
Run it inside the isolated desktop with:

```sh
./scripts/test-gnome.py --output build/qa/viewer-desktop -- \
  ./build/tests/test-console-ui --output build/qa/viewer
```

Add `--dark --large-text --width 720` to exercise the narrow 200% text layout.
Use `--monitor 1920x1080@60` on the desktop wrapper and `--width 1600` on the test
executable to check that the controls stay at the content pane's left inset in
a wide window.

`test-console-connect` checks automatic display connection with synthetic SPICE
frames and `test:///default`. It covers socket/start/channel errors, a missing
first frame, retries beyond three failures, capped backoff, successful recovery,
stop/start events, and disposal with timers pending. A disabled-monitor fixture
withholds the first frame until the client enables the monitor and supplies a
size while connection status is still visible. It also covers an agent update
before mapping and a later agent update. It checks that initial configuration
waits for mapping and matches the host monitor's pixel resolution,
unchanged configuration counts after window resizing and agent notifications,
cropping after guest monitor geometry changes, and live enabling/disabling of
automatic resolution adjustment. Popup checks cover aspect fitting from the
visible monitor rather than QXL backing dimensions, refitting after guest mode
changes, retaining manual resizes on unchanged monitor notifications, fullscreen
restoration, rendered square guest corners in fullscreen with rounded corners
restored afterward, and cancelling a pending fit when returning to the collection.
The preference schema is compiled for local
builds and installed under `share/glib-2.0/schemas`. Its default is false; a
headless memory-backend check verifies defaults and shared setting values. An idle-QXL fixture withholds the first
frame until two adjacent pointer positions arrive; healthy connections send no
activation, and disposal cancels a pending activation. Its linker wrappers
also inject active/suspended window state and exercise the production GTK
notification handlers. Pixel checks cover repeated return from inactivity,
compositor suspension, and remapping with no new SPICE damage event. Repeated
surface loss checks verify no background pointer activation, foreground wake-up
without reconnecting or resizing, and timeout recovery when input is unavailable.
These synthetic checks do not reproduce a real guest's idle policy or certify
physical desktop focus handling. The timer wrappers
shorten reconnect/watchdog timers only in this executable; production delays
remain unchanged. Run it with:

```sh
./scripts/test-gnome.py --output build/qa/console-connect -- \
  ./build/tests/test-console-connect
```

Use `--monitor 1920x1080@60` on the desktop wrapper to check another initial
resolution. To check device pixels at 200% display scale:

```sh
./scripts/test-gnome.py --monitor 2560x1600@60 --output build/qa/console-connect-hidpi -- \
  ./scripts/qa-display-scale.py --scale 2 -- ./build/tests/test-console-connect
```

`./scripts/test-gnome.py --output build/qa/gnome` runs the UI matrix in
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

`--physical` uses the caller's unlocked GNOME Wayland display while retaining
the private bus and all private state directories. It checks the screen-lock
state before starting and leaves the user's compositor, display mode and power
profile unchanged. Keep the test windows unobscured. This mode requires `gdbus`
and a caller environment containing `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` and
the normal session bus address. Only processes with this run's private runtime
marker are eligible for cleanup. The report identifies the display type and
records no owned shell for a physical run.

The matrix includes Boxes Flatpak onboarding and viewer pop-out/return at normal
size and at 200% text/dark/720 px. Its fixtures check Flatpak-only rows, offline
instructions, refresh, dismissal, one-time behavior across windows, manual
reopening, import connection errors and retry, and first-run suppression after
uninstalling despite residual definitions. Native domains remain available without
any import. The synthetic
fixtures do not connect to your real guests.

Use `--repeat 2` to run the standard ten-case matrix twice in the same desktop
(20 cases). The lifetime case adds eight repeated window/dialog/console cycles.
The allowed range is 1–20; custom commands after `--` require the default of one.
Repeated cases have separate log names and screenshot directories, and each
case records elapsed seconds. The Boxes exercise observes the frame clock of
each replacement window and waits for that window's first paint before driving
it. `test-ui --boxes-exercise --trace` logs step transitions for diagnosis. See
the [readiness investigation](../research/boxes-readiness/README.md) for the
reproduced harness defect and the still-open historical timeout.

Two `--boxes-deferred-exercise` cases hold the real discovery worker at a
FIFO-backed fake `flatpak info`. They pause and resume a synthetic test-driver
machine while discovery is pending, then open Preferences (normal size) or
creation (200% text/dark/720 px). Releasing the scan must leave that dialog in
place and offer the import guide after it closes. A linker wrapper observes
scan completion while forwarding the actual result; it does not replace worker
scheduling. Fixtures live under the private desktop runtime directory. This
adds no application polling or production timeout changes.

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
viewer. The matrix above continues to use synthetic VMs only.

The same tool supports profiling without `--exercise`; the Python standard-library
wrapper `scripts/profile-ui.py` creates isolated 0–1,000-domain fixtures and saves
raw measurements. See the performance design for methodology.
Use `--presentation` with GTK 4.24+ for complete compositor feedback. The separate
`--application --idle-seconds 0` mode observes the unchanged production entry
point inside the isolated wrapper; its preload module is never installed.
See the [performance workflow](../design/performance.md) for commands, conditions
and the distinction between wall time, thread CPU and presentation.

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

## Windows guest checks

Windows profiles add optional runtime dependencies `edk2-ovmf`, `swtpm` and
`swtpm-tools`; no additional library is linked into Pirwa. These packages are
recommended by the Fedora recipe and required for the Windows 11 workflow.
The guest installation and VirtIO driver ISOs are supplied by the user.

```sh
meson test -C build config windows-provision --print-errorlogs
./scripts/test-gnome.py --output build/qa/windows-create -- \
  ./build/tests/test-create-ui --output build/qa/windows-create/screenshots
./scripts/test-gnome.py --output build/qa/windows-create-accessible -- \
  ./build/tests/test-create-ui --output build/qa/windows-create-accessible/screenshots \
  --dark --large-text --width 720
```

`windows-provision` uses real `test:///default` domain/storage metadata with
synthetic capabilities and ISO identification. It checks pre-allocation rejection,
creation, failed-start rollback, and removal with TPM/firmware state preservation.
Its adapters create tiny private fixture files and record undefine flags; the
synthetic driver has no actual firmware or TPM process. Neither check certifies
Windows installation, Secure Boot execution, driver signatures, or guest networking.
See the [validation record](../research/windows-guests/README.md) and
[installation workflow](../user/windows.md).

## Proxy networking checks

The proxy gateway adds `libslirp-devel` (libslirp >= 4.7); the runtime is linked
only by the separate `pirwa-network` helper, not the GTK executable. Install
`gsettings-desktop-schemas` for GNOME proxy settings. Python 3 is used only by the
packet test. The Fedora recipe and CI install these dependencies.

```sh
meson test -C build proxy-settings network-definition network-start network-gateway --print-errorlogs
```

`proxy-settings` uses private schemas and the memory settings backend.
`network-definition` checks temporary userspace, bridge and virtual-network NIC
substitution, adapter identity, saved-definition preservation and custom-network
rejection with `test:///default`.
`network-start` runs the production helper launcher with the test driver and a
synthetic QEMU socket, including bridged/network adapters, rejection without a
direct fallback, cancellation and startup failure. The Python
gateway test supplies Ethernet frames directly to the actual helper and uses
local SOCKS5/HTTP fixtures. No test contacts a public resolver, changes desktop
settings, needs root/KVM/a display, or connects to an existing libvirt daemon.
Real QEMU/SELinux and guest browser acceptance remain integration checks.

The explicit desktop fixture checks the complete start error, wrapping instead of
ellipsis, the Preferences button, retry and dismissal without changing settings:

```sh
./scripts/test-gnome.py --output build/qa/network-desktop -- \
  ./build/tests/test-network-ui --output build/qa/network
```

Add `--dark --large-text --width 720` to check the narrow 200% text layout.
This fixture uses a stopped test-driver VM and injects the production network
compatibility error; it never opens a real guest or changes desktop preferences.

During the earlier proxy validation, the matching Fedora
`libslirp-devel-4.9.3-2.fc45.x86_64` RPM was extracted to
`/tmp/pirwa-proxy-sdk`; its pkg-config prefix and linker symlink reference the
installed runtime. This is the same temporary SDK technique described below,
not an installed system package or a durable build requirement. Include its
`usr/lib64/pkgconfig` directory alongside the original SDK paths when
reconfiguring this host's builds. Install the normal development package on a
durable build host. That temporary directory is now gone; the current local
workaround is the persistent SDK described under launcher recovery above.

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

The earlier implementation also extracted matching libvirt-glib-devel and libosinfo-devel
RPMs into the same temporary prefix. GCC libasan/libubsan runtime RPMs were
extracted to run address/undefined checks without changing system packages.
For those local sanitizer builds, pass the SDK library directory as a linker
search path and `LD_LIBRARY_PATH`. Current headless checks use leak detection
and the narrow exception above. Historical runs used `detect_leaks=0`.

Install headers normally on a durable development host. The earlier temporary
prefix no longer exists; the current `build` uses `.cache/dev-sdk` as described
under launcher recovery above.

The Fedora RPM recipe is `packaging/pirwa.spec`. Create a source tarball from the
committed tree with `meson dist -C build --no-tests` and place its xz archive in
the RPM SOURCES directory. Pass the Meson source version explicitly:

```sh
pirwa_version=$(meson introspect --projectinfo build | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
rpmbuild --define "pirwa_source_version $pirwa_version" -ba packaging/pirwa.spec
```

Install BuildRequires first. The RPM base Version and AppStream release version
must agree with Meson; the headless `release-metadata` check enforces that, along
with desktop/icon/schema IDs and screenshot dimensions. CI and the rootless
package check use the same source-version override. Before a stable tag, update
the Meson version, AppStream release kind/date and RPM Version/Release together.
The native Meson install path also works without RPM. For the common DEB,
Flatpak bundle, complete GitHub release workflow and COPR CLI publishing, use
the [release packaging guide](../release/publishing.md). Both native recipes
include the embedded avatar artwork's license and attribution.

AppStream validation is strict and offline, with no severity override. Both the
staged install and RPM build validate the complete desktop/metadata/icon tree.
Project URLs point at `https://github.com/stonega/pirwa`, which is currently
private. Public page and screenshot reachability still needs checking after
publication. See [metadata readiness](../research/release-metadata/README.md).

## Native SPICE development headers on this host

The console adds `spice-glib-devel` and `spice-protocol`, with GTK 4.16 required
for memory textures with damage hints. GTK 3 headers are not used. During the earlier console validation,
the signed matching Fedora `spice-glib-devel-0.43-5.fc45.x86_64` and
`spice-protocol-0.14.5-5.fc45.noarch` RPMs were extracted into the existing temporary
SDK prefix. The local SPICE `.pc` omits `Requires.private` (only the shared library
is linked; its runtime dependencies are installed) and its linker symlink points
to the installed runtime. This is a local SDK workaround, not a repository build
requirement. Normal `dnf install spice-glib-devel` supplies all development dependencies.

For the current local SDK, both pkg-config directories are needed:

```sh
pirwa_sdk="$PWD/.cache/dev-sdk/usr"
meson setup build --reconfigure --clearcache \
  -Dpkg_config_path="$pirwa_sdk/lib64/pkgconfig,$pirwa_sdk/share/pkgconfig"
```
