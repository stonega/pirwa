# Performance contract

These are initial engineering targets. They are not benchmark results and do
not establish that Pirwa is faster than Boxes. M0 must record a named reference
machine, distribution, kernel, display server, renderer, dependency versions,
power profile, and the exact release build before accepting or revising them.

## Implementation measurements

See [recorded results](../research/benchmarks.jsonl) for the release-build
0/100/1,000-domain synthetic bulk-read samples. The 1,000-domain run measured
p95 12.898 ms. This is not a real-daemon, first-frame, or guest benchmark.
Native desktop measurements and remaining gaps are recorded in
[implementation validation](../research/v0.1-validation.md). Partial measurements
do not establish the complete release budgets below.

The [console/performance follow-up](../research/console-performance/README.md)
records 30 warm launches each at 100 and 1,000 domains, a 60-second idle sample,
scroll CPU work, and 30 paired real-KVM starts. The 100-domain run measured
p95 496.378 ms through after-paint, 96.1 MiB idle PSS, and 0.0019% idle CPU.
Start backend overhead was p95 27.127 ms over persistent direct libvirt.
Presentation and cold-cache gates remain separate. Guest storage results are
inconclusive: substantial variation also occurred with the manager closed.

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
3. Run `GDK_BACKEND=wayland ./scripts/profile-ui.py --domains 100 --launches 30
   --idle-seconds 60 --output build/qa/native-ui-100.json` on a quiet desktop
   (enter this as one line). The Python standard-library wrapper generates isolated
   libvirt test-driver XML. It warms once, saves every launch sample, then samples
   the production window/backend with normal GTK animations and accessibility.
   It records all-thread process CPU, `/proc/self/smaps_rollup` PSS, passive frame
   counts, renderer, monitor refresh/scale, source revision, binary hash, packages,
   power profile, and host load. Do not interact with the windows during sampling.
4. Use `--domains 1000 --scroll-seconds 5` for a large collection. The harness
   scrolls 40 pixels per frame and records before-paint to after-paint CPU work.
   That span excludes compositor presentation and GPU completion. Startup spans
   end at after-paint or connection readiness in a non-unique QA application;
   they do not include production single-instance bus activation. No cold-cache
   claim is made. Synthetic domains live inside the process, unlike a real daemon.
   Measure a real QEMU session separately. Use Sysprof/presentation traces for
   actual frame latency, allocations, backend stalls, and wakeups. Those gates
   remain open; a CPU work sample alone does not pass the frame-time budget.
5. For guest throughput compare the same domain XML and guest workload launched
   with Pirwa and direct libvirt/virsh, in alternating repeated runs. Record
   vCPU/RAM, CPU mode, KVM, storage format, I/O mode, network backend, video, and
   host cache conditions. Test manager-only and console-attached scenarios.

Store results in `docs/research/` with Git commit, exact commands, raw samples,
sample count, domain count, and environment. Re-run affected benchmarks when
backend, collection, provisioning defaults, or display code changes. Never add
arbitrary sleep-based timing assertions to the correctness test suite.

`GSK_RENDERER=cairo` can be used for a separate renderer comparison. Keep its
results separate from the default renderer and record scrolling CPU as well as
startup/memory. Pirwa leaves renderer selection to GTK. Idle measurements start
with keyboard focus on the collection; focusing search deliberately activates
the normal text cursor animation. The connecting spinner stops when its page
is hidden. The profiling harness adds no tick callback during idle sampling.

For measurements without interference from the active desktop, the virtual
GNOME wrapper accepts a command after `--`. For example, enter this as one line:

    ./scripts/test-gnome.py --output build/qa/gnome-profile -- env GTK_A11Y=test ./scripts/profile-ui.py --domains 100 --launches 30 --idle-seconds 60 --output build/qa/gnome-profile/native-ui.json

Keep virtual-desktop results separate from physical-display results. This
example explicitly uses GTK's test accessibility backend because AT-SPI service
activation is unavailable in the isolated host session; that setting is
recorded in the raw profile. It is not a production accessibility measurement.

## Design constraints

Bulk RPCs, lifecycle events, virtualized rows, lazy details, bounded background
work, and no collection thumbnails are the starting choices. Measure them. The
persistent connection is reused for incremental updates; only an explicit
reconnect or a lost connection reopens it. Optimizing GTK startup cannot speed
up the guest's boot process.
