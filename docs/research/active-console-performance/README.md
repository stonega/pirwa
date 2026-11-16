# Active guest console profiling

This opt-in workflow runs controlled drawing inside a disposable Fedora
Workstation Live 43 guest. It measures the production `PirwaConsole` and
`PirwaDisplay` components through real libvirt graphics sockets and SPICE
decoding. The main collection window and audio are outside this measurement.

## Method

`tests/benchmark-console.c` links production UI/backend code. Linker wrappers
instrument only this executable: SPICE invalidation counts, CPU texture-copy
bytes/time, process CPU, PSS and passive GTK before-paint/after-paint spans.
Frame samples also record full/tile copies, cache compactions, texture appends,
the preceding frame's texture appends and texture-copy CPU time. Appends count
scene construction in Pirwa, not GPU draw calls. The first preceding-frame count
in each trial is zero. Replacing cached tile copies with a full copy is counted
as a compaction; trials that change the primary surface are rejected separately.
It adds no frame-driving callback. The application binary has no profiling
wrappers or additional polling.
The explicit profiling tools also retain GDK frame timing objects for measured
updates and export a separate `frame_feedback` object. It records before/after
paint, first decoded-damage receipt, completion, presentation time and result.
Only complete `presented` results identify accurate compositor feedback;
unavailable timestamps remain zero, and pending/discarded results remain in the
raw data. Result classification needs GTK 4.24; earlier supported GTK versions
record `unknown` and cannot establish this presentation metric. Virtual desktop
feedback describes a virtual output, not physical scanout or guest-to-screen
latency. The app does not collect these diagnostics.
The fixture also overrides SPICE's audio getter to keep agent volume queries
out of these display measurements.

The control forces full-frame invalidation in the same executable. It exercises
the whole-copy rendering path, including full GTK damage hints. The paired
comparison describes that control and the normal tile path. It does not reuse
the earlier synthetic benchmark's saved baseline executable.
For a renderer change, `--baseline-binary /path/to/saved/benchmark-console`
instead compares two native binaries. Each trial opens a fresh client, waits for
its agent and waits for the framebuffer to settle. Variant order alternates by pair, and
both binary hashes are recorded. Save the baseline executable and its source
manifest before rebuilding; never compare binaries with different sample formats.
Rebuild both sides with the same profiling fixture, including
`qa-frame-feedback.c`, when validating the new feedback fields. A matching
paint-column count alone does not establish matching instrumentation.

The guest uses a fullscreen GTK 3 drawing area with three workloads:

- `idle`: a static opaque image, with no animation timer.
- `small`: a changing 64×64 logical-pixel rectangle, requested every 33 ms.
- `full`: changing horizontal bands across the window at the same requested rate.

GTK 3 permits the small workload to request a damaged rectangle. The guest
compositor and SPICE server determine the actual transmitted regions; raw
invalidation counts and summed region areas retain that distinction. Guest
status records actual draw counts, allocation and device scale. Requested
cadence is not a presented-frame claim.

The historical measurements below used a three-second warmup. Current runs
require the agent to be ready at the expected size, with no ready-file change
for five seconds, within a 30-second deadline. The file is rewritten on primary
surface and agent updates, so a resize restarts that quiet window. Per-trial
warmup records retain elapsed time and observed ready-file versions. Any primary
change during measurement still rejects the trial. This polling is test-only
and ends before sampling. Check the readiness guard with
`python3 tests/test-console-profile.py` (no guest or display required).
Variant order alternates
by pair. QEMU process CPU is sampled separately over a recorded observer interval
that includes the control/readback calls surrounding each console sample.
Frame-clock spans end at GTK submission, excluding GPU completion and physical
presentation. The invalidation-to-after-paint span starts at delivery of a decoded
SPICE invalidation; guest rendering and earlier encode/decode latency are outside
that span. Texture CPU time includes allocation, copying/conversion and builder
work. Total process CPU also includes SPICE and profiling overhead.

The runner pins both executables before creating resources, records source/binary
and ISO hashes, and waits for the guest agent before injecting setup commands.
The test-only argv bridge uses a dedicated virtio channel; it has no network
listener. A read-only tools CD provides the guest scripts. No guest disk is
formatted or installed. The fixture retains Pirwa's normal four-vCPU, 4096 MiB,
16 GiB, KVM/UEFI/software-SPICE defaults.
`--memory-mb` can select 2048–8192 MiB for the disposable fixture only; it does
not change application defaults. Keep that setting identical in paired trials.

## Results — 2026-11-14

Fedora 45, Intel Core Ultra 9 185H, GTK 4.24.1, SPICE GLib 0.43,
GNOME Shell/Mutter 51.1, Mesa 26.2.6 and the observed balanced power profile.
The client uses Vulkan and scale 1 in separate 60 Hz virtual desktops. Both
client variants use GTK's test accessibility backend. Guest scale is also 1.
Executables are release builds (`-O3`, `b_ndebug=true`).

There are three ten-second trials per workload/path/resolution: 36 trials total.
CPU, copied bandwidth and update cadence below are medians of three trials.
Paint p95 uses the pooled frame samples and nearest-rank calculation. Every
arrow is **forced full-frame control → normal tile path**.

| Source / workload | Client CPU, % of one core | CPU texture copies, MiB/s | GTK updates/s | Paint span p95, ms |
| --- | ---: | ---: | ---: | ---: |
| 1920×1080 / idle | 0.0058 → 0.0042 | 0.0 → 0.0 | 0.0 → 0.0 | — |
| 1920×1080 / small | 45.98 → 9.54 | 413.6 → 29.4 | 52.3 → 57.0 | 17.15 → 2.30 |
| 1920×1080 / full | 59.24 → 52.48 | 439.8 → 324.9 | 55.6 → 57.2 | 10.12 → 9.97 |
| 3840×2160 / idle | 0.0059 → 0.0051 | 0.0 → 0.0 | 0.0 → 0.0 | — |
| 3840×2160 / small | 83.15 → 6.56 | 1051.1 → 15.0 | 33.2 → 58.2 | 30.17 → 1.61 |
| 3840×2160 / full | 93.55 → 86.16 | 1078.4 → 767.3 | 34.1 → 40.5 | 29.24 → 36.19 |

The real small-update workload confirms the sparse-path benefit. Dense 4K still
uses 86.16% of one client CPU core. Its p95 paint span rises to 36.19 ms, versus
29.24 ms for the control, despite lower total CPU and more GTK updates. This
remaining tail-cost issue needs investigation before drawing a general latency
conclusion. Texture creation/copying itself accounts for 15.65% of one core in
the normal dense 4K case; the remaining process work needs a CPU/GPU trace.

The guest draws about 30.1 times/s in the small and 1080p full workloads, and
about 18.1 times/s in the 4K full workload, with both client paths. SPICE regions
can trigger multiple GTK updates per guest draw. The GTK update column therefore
does not establish guest or physical-display FPS. QEMU's median process CPU in
the dense 4K case is about 279% of one core with tiles and 272% with the control;
that includes guest execution and QEMU's display work over its separate observer
interval. This run does not isolate a guest CPU or storage regression threshold.

Static-client CPU medians are below 0.01% of one core, with no texture copies or
guest paint updates. Each path has 30 seconds of idle samples at each resolution.
Idle PSS is approximately 73 MiB at 1080p and 159 MiB at 4K for the tile path.
These figures cover the instrumented console component with audio disabled.

Every raw frame sample and environment/cleanup record is retained in
[1080p.jsonl](1080p.jsonl) and [4k.jsonl](4k.jsonl). The first record is environment
metadata; following records are individual trials. [validation.json](validation.json)
records the 12 normal checks, 12 address/undefined sanitizer checks (leak
detection disabled for the known libvirt initialization finding), seven manager
UI cases, opt-in guards, and cleanup. Both final fixtures, volumes and pools were
removed; their existing-domain baselines match and private desktops have no
remaining processes. An earlier setup attempt recorded an external removal from
the existing inventory; that change was preserved, and those failed attempts are
excluded from these measurements.

## Reproduction

Requirements beyond a normal development build: local session libvirt/KVM,
`virsh`, `genisoimage`, Python GObject bindings, GNOME Shell, and the Fedora live
ISO. These are opt-in QA dependencies. Default Meson tests need no VM or display.

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf benchmark-console qa-vm
pirwa_qa_uri="qemu+unix:///session?socket=${XDG_RUNTIME_DIR}/libvirt/virtqemud-sock"
PIRWA_RUN_KVM_TESTS=1 ./scripts/test-gnome.py --monitor 1920x1080@60 \
  --command-timeout 900 --output build/qa/active-console-1080-desktop -- \
  ./scripts/profile-console.py --uri "$pirwa_qa_uri" \
    --iso /path/to/Fedora-Workstation-Live-43-1.6.x86_64.iso \
    --expect-size 1920x1080 --runs 3 --seconds 10 \
    --output build/qa/active-console-1080
```

Use a fresh output directory. The explicit host-session socket avoids opening a
different libvirt session beneath the private runtime. Adjust the socket for the
host's daemon configuration. Use `3840x2160@60` and `--expect-size 3840x2160` for
4K. Actual primary dimensions are checked; suspended or resizing trials fail.
Before creating its output or any guest resources, the runner requires
`MemAvailable` to cover the selected guest RAM plus 1024 MiB of host headroom.
This check runs inside the private desktop. It prevents starting the fixture
when the host is already short of memory; it does not reserve memory or guarantee
that later pressure cannot occur. Per-trial pressure records still need review.
Use `--modes full,small` to focus a follow-up, or `--modes full` for a diagnostic.
`--setup-seconds 45` allows a slow live welcome screen to settle before setup
input (5 seconds by default, permitted range 5–60). This wait is outside sampling.
New trials record host CPU, memory and I/O pressure before/after sampling so
host stalls can be distinguished from renderer work. Loaded-host results need
separate interpretation and do not replace the historical dataset above.

The runner creates a random `pirwa-qa-UUID` guest and pool, then stops/removes its
fixture and verifies that the domain, volume and pool are absent. Reports include
before/after hashes and states of other definitions. The C executable separately
checks the explicit opt-in, private desktop runtime and disposable domain name
before connecting. Setup keyboard commands target only the newly created QEMU
UUID through the QA runner. The existing `qa-vm` helper independently
enforces its disposable identity and verified volume ownership for removal.

`results.json` contains every trial and frame sample. The private desktop wrapper
records its own process cleanup in `process-cleanup.json`. Failed boot/control
runs attempt a guest screenshot and retain their cleanup results for diagnosis.
The desktop wrapper sends SIGTERM on a command timeout and allows 30 seconds for
cleanup before forcing an unresponsive command to stop; the timeout remains a
failed test.

## Next performance work

Trace dense 4K paint-time spikes and correlate them with tile count, compaction,
SPICE decode and GPU submission. Compare a bounded/adaptive dense-update path
and native accelerated scanout while preserving sparse-update gains. Broader
application workloads and physical presentation remain release gates.
The [dense-console follow-up](../dense-console-performance/README.md) records
the packed RGB copy change, per-frame counters and rejected tuning experiments.
The [rounded renderer follow-up](../renderer-memory/README.md) records the
direct-clip comparison and subsequent compositor-feedback validation.

## Acceptance limits

This is one host/guest combination with controlled GNOME drawing. Broader guest
applications, video, accelerated scanout, physical input/audio and presentation
latency remain separate work. Source resolution and guest logical resolution
must stay distinct. The existing [synthetic renderer record](../display-performance/README.md)
remains a separate dataset.

References: [SPICE primary surfaces and invalidations](https://www.spice-space.org/api/spice-gtk/SpiceDisplayChannel.html),
[GTK frame-clock phases](https://docs.gtk.org/gtk4/drawing-model.html).
