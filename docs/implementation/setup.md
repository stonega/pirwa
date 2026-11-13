# Local development

## Requirements

Meson ≥ 1.3, a C17 compiler, Ninja, pkg-config, GLib/GIO ≥ 2.76, GTK ≥ 4.12,
libadwaita ≥ 1.4, and libvirt ≥ 9.0. Development headers are required, not just
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
for automated tests. Real-KVM tests are a later, explicitly opt-in workflow.

`examples/run-test-driver.sh` launches the UI with synthetic domains on your
normal desktop. `PIRWA_LIBVIRT_URI` affects only a newly launched application
process; an already-running single-instance app keeps its connection.

`scripts/benchmark.sh [build-directory]` runs the headless inventory benchmark.
It uses the built-in test driver and does not touch real guests.

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
