# Lifecycle and sanitizer startup follow-up

On 2026-11-16, leak-enabled checks found and reproduced a Pirwa backend lifetime
defect. It is fixed, and all 20 headless checks pass normally and with
ASan/UBSan plus LeakSanitizer. A fresh offline Fedora build also builds,
installs and verifies the development RPM. This does not establish full GUI leak
freedom or release readiness.

## Backend reference leak

The real libvirt test driver accepts close-callback registration without storing
the callback or invoking its opaque-data destructor. Pirwa retained one backend
state reference on that successful no-op. Stopping and reconnecting synthetic
backends therefore retained the state, its queue, cancellation object and URI.
Joining the worker and draining the main context did not release them.

The [libvirt API](https://libvirt.org/html/libvirt-libvirt-host.html#virConnectRegisterCloseCallback)
limits close notifications to persistent transports. Its
[12.6.0 implementation](https://github.com/libvirt/libvirt/blob/v12.6.0/src/libvirt-host.c#L1414)
returns success when a driver has no registration method. Pirwa now registers
close notifications only for QEMU, the persistent transport among its supported
drivers. Lifecycle and tray events retain their existing ownership. Fault tests
explicitly model a QEMU transport while retaining real test-driver domain
operations. No production thread join, new dependency or polling was added.

`backend-lifetime` observes queue release and backend finalization after worker
exit. It checks 16 rounds each of connect/reconnect, failed connection and stop
before opening. Its queue-release assertion fails against the old backend
(`1 == 0`) and passes with the fix. The existing seven transport-fault cases
still pass. Private QEMU tests also pass startup recovery, graceful/crash daemon
replacement, socket denial/recovery and shutdown during backoff with leak
detection enabled and a clean client exit. These operate only an empty private
daemon; no existing guest or system service is changed.

## Narrow dependency exception

An independent program containing only `virConnectOpen("test:///default")`
and `virConnectClose` reproduces 263 leaked bytes in eight allocations:
four `virDataTypesOnce` class initializations and their names. The exception in
`tests/lsan.supp` matches only that initialization stack. It does not match
Pirwa backend allocations or general libvirt, GLib, GTK or allocator functions.
The independent control passes with the exception; a separate intentional
17-byte application leak still fails LeakSanitizer.

CI and the clean Fedora runner now enable leak detection using this exception.
All 20 checks pass in both the local sanitizer build and the clean container.
Private-daemon runners now forward `LSAN_OPTIONS` and `DEBUGINFOD_URLS` to the
isolated client alongside the existing sanitizer settings. The first private
run completed all five functional checks but failed on the known 263-byte
initialization leak because the runner had dropped `LSAN_OPTIONS`. The corrected
runner passes functional checks and client shutdown; the failed run is retained.

The GUI leak run remains a finding: the GL-renderer action exercise reports
475,017 bytes in 11,805 allocations, with Fontconfig, GTK and graphics-driver
allocation stacks. No broad GUI suppression or global toolkit cleanup was
added. These results require further assessment before GUI leak acceptance.

## Current sanitizer startup timeout

Normal UI checks pass nine cases after the backend fix; the earlier three-repeat
run passed 27 cases. Separate sanitizer runs reproduce a zero-size mapped window
with no first frame and no inventory readiness before the unchanged watchdog.
A minimal libadwaita/GTK program with no Pirwa or libvirt code reproduces the
same watchdog failure during `gtk_window_present`.

A diagnostic stack captured the main thread in ASan's realloc interceptor,
called from `libnvidia-glcore.so.615.71.09` and `libGLX_nvidia.so.0`. A separate
GL-renderer control and an explicit Intel Vulkan driver control complete. The
untraced minimal GTK control spent 25,455.832 ms in presentation with ASan and
default driver discovery, versus 28.774 ms with the Intel-only override. The
normal controls recorded 211.398 ms and 18.236 ms respectively. A separate
debugger run captured the stack above. These are single-run diagnostic controls,
not performance acceptance or an application improvement.

The [Vulkan loader's documented driver override](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#overriding-the-default-driver-discovery)
allows limiting this diagnostic to the Intel ICD. With that explicit override,
18 sanitizer UI cases pass using Vulkan. Their leak detection remains disabled
because GUI leak assessment is separate. The application continues to use GTK's
default renderer and driver discovery. QA desktop/profile reports now record
explicit graphics-selection environment values so controlled-driver results
cannot be confused with default-driver acceptance.

This establishes a graphics-driver/sanitizer startup stall on this host. It
does not prove the cause of the older uninstrumented timeout or the historical
Boxes step-4 timeout. Those historical findings remain open, as do production
startup/scrolling/memory budgets and physical desktop acceptance.

## Reproduce

After configuring the normal and sanitizer builds:

```sh
meson test -C build --print-errorlogs
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp:print_suppressions=1" \
  meson test -C build-release-review --print-errorlogs
PIRWA_RUN_DAEMON_TESTS=1 ASAN_OPTIONS=detect_leaks=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp:print_suppressions=1" \
  ./scripts/test-libvirt-daemon.py --build build-release-review \
  --output build/qa/leak-private-qemu-new
```

On this Intel/NVIDIA host only, the explicit Vulkan control is:

```sh
ASAN_OPTIONS=detect_leaks=0 \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json \
  ./scripts/test-gnome.py --build build-release-review --repeat 2 \
  --output build/qa/readiness-intel-new
```

Use the SDK runtime loader path on this development host; see
[setup](../../implementation/setup.md). The ICD filename is host-specific. These
commands do not change the application renderer policy or desktop settings.

[Validation](validation.json) retains source/binary hashes, exact outcomes,
the pre-fix regression, independent controls and scope. The
[compressed raw results](raw-results.json.gz) and
[source archive](sources.tar.gz) accompany it. Full local artifacts remain under
`build/qa/leak-assessment-20261116/`, `build/qa/leak-private-qemu-fixed-20261116/`,
`build/qa/readiness-intel-sanitize-20261116/` and
`build/qa/lifecycle-release-20261116/`. The frozen package snapshot predates later
QA environment-recording changes; its report records that source drift.
