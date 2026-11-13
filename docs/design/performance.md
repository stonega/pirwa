# Performance contract

These are initial engineering targets. They are not benchmark results and do
not establish that Pirwa is faster than Boxes. M0 must record a named reference
machine, distribution, kernel, display server, renderer, dependency versions,
power profile, and the exact release build before accepting or revising them.

## Implementation measurements

See [recorded results](../research/benchmarks.jsonl) for the release-build
0/100/1,000-domain synthetic bulk-read samples. The 1,000-domain run measured
p95 12.898 ms. This is not a real-daemon, first-frame, or guest benchmark.
The complete release budgets below remain provisional and unverified.

## Provisional release budgets

| Measure | Target | Conditions |
| --- | --- | --- |
| Launch to first rendered frame | p95 ≤ 500 ms warm; ≤ 1.5 s cold | At least 30 launches per mode; UI appears before connection completes |
| Inventory | p95 ≤ 300 ms at 100 domains; ≤ 1 s at 1,000 | Healthy already-running local daemon; separate cold connection results |
| Collection frame time | p95 ≤ 16.7 ms | 60 Hz reference display, 1,000 domains, continuous scroll |
| Main-thread stalls caused by backend | No blocking libvirt/filesystem work | Sysprof trace under slow/disconnected daemon and import workloads |
| Idle process CPU | Average < 0.5% of one core over 60 s | No console, no active job, stable collection; record baseline system noise |
| Idle process PSS | ≤ 100 MiB | 100 domains, one connection; excludes daemon, viewer, and QEMU processes |
| Hidden collection work | No periodic stats or screenshot work | Confirm with trace and wakeup counts |
| Selected VM telemetry | At most one bulk sample per 2 s | Only while resource details are visible; none when hidden |
| Start-command overhead | p95 ≤ 250 ms over equivalent direct libvirt start | Same domain; excludes guest boot and display connection |
| Guest CPU/disk regression | ≤ 3% CPU, ≤ 5% storage against same XML through virsh | Repeated paired runs, same guest image/cache state, manager idle |

Treat CPU/disk figures as investigation thresholds subject to measurement noise.
Cold first-frame and connection timing must be separated. Do not infer display
performance or guest boot speed from inventory microbenchmarks.

## Measurement workflow

1. Build optimized code with symbols: `meson setup build-perf --buildtype=release -Db_ndebug=true`.
2. Run `./scripts/benchmark.sh build-perf`. It measures 25 complete read-only
   inventory requests against `test:///default`, including connect/close. Its
   JSON reports the actual domain count and p50/p95 wall time. This is a small
   development baseline, not a QEMU or UI benchmark.
3. In M0 add isolated test-driver fixtures for 0/100/1,000 domains and save raw
   samples. Measure a real QEMU session separately; synthetic domains do not
   reproduce daemon I/O, storage, or graphics costs.
4. Use Sysprof for process start, first frame, scroll frame times, allocations,
   and main-thread stalls. Mark inventory/command spans in M0. Measure process
   PSS using `/proc/<pid>/smaps_rollup`; report renderer and viewer costs separately.
5. For guest throughput compare the same domain XML and guest workload launched
   with Pirwa and direct libvirt/virsh, in alternating repeated runs. Record
   vCPU/RAM, CPU mode, KVM, storage format, I/O mode, network backend, video, and
   host cache conditions. Test manager-only and console-attached scenarios.

Store results in `docs/research/` with Git commit, exact commands, raw samples,
sample count, domain count, and environment. Re-run affected benchmarks when
backend, collection, provisioning defaults, or display code changes. Never add
arbitrary sleep-based timing assertions to the correctness test suite.

## Design constraints

Bulk RPCs, lifecycle events, virtualized rows, lazy details, bounded background
work, and no collection thumbnails are the starting choices. Measure them. The
initial refresh reconnects; M1 introduces connection reuse and incremental
updates. Optimizing GTK startup cannot speed up the guest's boot process.
