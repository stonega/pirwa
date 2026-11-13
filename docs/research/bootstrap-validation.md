# Bootstrap validation — 2026-11-13

This records the initial scaffold, before the first commit. It does not validate
v0.1 functionality or the provisional release performance budgets.

## Environment

- Fedora Workstation 45, Linux `7.2.15-300.fc45.x86_64`.
- Intel Core Ultra 9 185H, 22 logical CPUs.
- GCC 16.2.1, Meson 1.12.1, Ninja 1.13.2.
- GLib 2.90.1, GTK 4.24.1, libadwaita 1.10.0, libvirt 12.6.0.
- Matching libvirt development RPM extracted locally; see
  [setup](../implementation/setup.md#bootstrap-host-note).

## Passed

- `meson setup build` and `meson compile -C build`, with strict warnings.
- `meson test -C build --print-errorlogs`: desktop-file validation and the
  inventory suite. Its three cases cover real test-driver discovery and caller
  thread callback delivery, pre-cancellation, and connection failure.
- `clang-format --dry-run --Werror src/*.[ch] tests/*.c`.
- `sh -n scripts/benchmark.sh examples/run-test-driver.sh`.
- `./build/src/pirwa --version`: `Pirwa 0.1.0-dev` without a display.
- Native GTK UI rendered through GTK Broadway on loopback with a separate
  headless Chrome profile. Inspected the name/state row and refresh result;
  closed the window through its control and verified process exit. A row
  compatibility issue was corrected during the check. This does not replace
  real Wayland/KVM testing. See [rendered scaffold](scaffold.png).
- Authorized CodeGraph initialization: initial result 9 indexed files, 63 nodes,
  117 edges. The local database is excluded from Git.

## Inventory microbenchmark

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
./scripts/benchmark.sh build-perf
```

One observed run:

```json
{"driver":"test:///default","domains":1,"warm_samples":25,"first_request_ms":4.405,"warm_p50_ms":0.763,"warm_p95_ms":1.066}
```

This times the complete asynchronous read-only inventory request, including
connect and close, against one synthetic domain. It measures neither first-frame
time nor a real QEMU daemon, guest execution, or console rendering. M0 must add
the larger workloads, raw sample capture, environment controls, and reference
comparisons in the [performance contract](../design/performance.md).

## Memory-check limits

The AddressSanitizer/UndefinedBehaviorSanitizer build could not link because
this host lacks GCC's `libasan` and `libubsan` runtime packages. It was not passed.

With `DEBUGINFOD_URLS=` to avoid blocking symbol downloads, Valgrind completed
the three inventory tests. It reported 192 definitely lost bytes plus 71 indirect
bytes in four `virClassNew` initialization allocations, and 1,536 possibly lost
bytes from GLib worker-thread TLS. The strict leak-check command exits nonzero.

An independent C program doing only `virConnectOpenReadOnly("test:///default")`
and `virConnectClose` reproduces exactly the four allocations (192 direct plus
71 indirect bytes), with no Pirwa code or worker threads. This points to the
installed libvirt test-driver initialization, rather than inventory snapshots.
No broad suppressions were added. A clean sanitizer run and further dependency
memory investigation remain part of the M0/M4 gates; this is not a clean memory
certification.
