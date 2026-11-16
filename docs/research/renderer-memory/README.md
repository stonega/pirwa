# Rounded rendering and retained memory

Measured on 2026-11-15, following the
[dense-console investigation](../dense-console-performance/README.md).
All trials use an isolated GNOME desktop and synthetic framebuffers. They do
not connect to libvirt or alter existing VMs.

Reference host: Intel Core Ultra 9 185H, Fedora 45, Linux 7.2.15,
glibc 2.44, GTK 4.24.1, GNOME Shell/Mutter 51.1 and the balanced power profile.
The compiler is GCC with `-O3 -g -std=c17`; exact versions and hashes are in the
manifests. The source checkout was dirty at `f332a02`; the retained snapshots
identify the measured source independently of that commit.

## What changed

The 12 px desktop radius is preserved. `pirwa-display.c` now uses a direct
rounded clip when there is one opaque texture, no replacement tiles, and its
aspect ratio exactly matches the positive widget allocation. ARGB, layered
updates and letterboxed views retain the luminance mask. Those cases need a
composited image to avoid blending rounded or fractional edges repeatedly.

The pixel oracle converts RGB content into an equivalent opaque ARGB reference,
forcing the reference through the mask even when the tested image takes the
direct clip. The existing maximum channel-error tolerance remains one level.

## Retained memory

The explicit Linux/glibc `benchmark-display-memory` executable tracks app-created
immutable textures with weak references and records `/proc/self/smaps_rollup`,
`mallinfo2`, allocator XML and DRM descriptor statistics. None of its observers,
sampling timers or allocator calls enter the app or the default test suite.

Two runs each of flat/row-copy, flat/packed-copy and rounded/packed-copy controls
performed three open/draw/idle/clear/close cycles: 18 viewer lifecycles at 4K,
with 32-pixel band updates. Every settled clear and cycle end had zero live
tracked texture bytes. Peak tracked texture payload was 71.0 MiB. Sampled peak
process PSS ranged from 157.7 to 200.7 MiB.

After the last viewer closed, a diagnostic-only `malloc_trim(0)` released
8.4–77.0 MiB of PSS across the six processes. For the two rounded controls,
PSS fell from 96.7/95.4 MiB to 54.1/54.1 MiB, with no live tracked textures
before trimming. This demonstrates substantial retention of free allocator
pages in these samples. A virtual free-heap total can remain large after resident
pages are discarded; it is not a live-allocation or resident-memory total.

This does not explain every earlier real-console PSS endpoint: SPICE decoding
is absent, and tracking app-created textures does not track all GTK, driver or
GPU resources. Three cycles do not establish leak freedom over an unlimited
session. No production trimming policy was added.

Raw samples and allocator/DRM aggregates: [memory.jsonl](memory.jsonl).
Source/binary hashes, environment, flags and protocol:
[memory-manifest.json](memory-manifest.json).

## Isolating the rounding cost

Before adding the fast path, two alternating pairs compared a flat control
against the rounded luminance mask, with identical packed-frame copies.
The flat control removes only the outer mask and retains transparent empty
states. Each trial warms for 30 updates and measures 60 updates at 3840×2160,
scale 1, fullscreen, using `GskVulkanRenderer` on a virtual 60 Hz monitor.

| Workload | Flat paint p95 | Masked paint p95 | Flat / masked process CPU |
| --- | ---: | ---: | ---: |
| Regenerate full framebuffer | 36.537 ms | 52.964 ms | 86.70% / 86.70% |
| Change 64×64 region | 1.287 ms | 1.610 ms | 5.78% / 7.23% |

CPU is percent of one core, median across trial means. p95 uses nearest rank
over 120 pooled frame spans per cell. The dense producer alone takes roughly
13 ms at p95, and the saturated processes submit different numbers of updates
per second. Equal CPU percentages do not imply equal rendering cost.
Raw trials: [mask-control.jsonl](mask-control.jsonl).

The subsequent `invalidate-full` workload initializes a gradient once, changes
one pixel each update and invalidates the entire framebuffer. It exercises full
texture creation and submission while excluding expensive synthetic image
generation. The direct and masked binaries use the same fixture, packed-copy
helper, compiler flags and dependencies. They differ only in the guarded
rounded-clip branch. Its separate paired results are recorded in
[summary.json](summary.json) and [direct-clip.jsonl](direct-clip.jsonl).

| Renderer-only full invalidation | Masked baseline | Direct clip |
| --- | ---: | ---: |
| GTK paint wall p95 | 41.237 ms | 27.343 ms |
| GTK paint CPU p95 | 40.545 ms | 26.862 ms |
| Process CPU, median (% of one core) | 86.28% | 84.02% |
| GTK updates/s, pooled | 24.43 | 37.03 |
| Producer CPU p95 | 0.018 ms | 0.017 ms |

Three alternating pairs contribute 180 measured updates per variant. Every
direct trial's p95 (24.715–27.964 ms) was below every masked trial's
(39.993–42.595 ms). Pooled paint p95 falls 33.7%; it still exceeds 16.7 ms.
The checked image's maximum channel error was one for direct rendering and zero
for the masked control. The [comparison manifest](direct-clip-manifest.json)
records the source and binary hashes, environment and profiler settings.

Frame spans include GTK Update through after-paint. They exclude compositor
presentation, GPU completion, SPICE decoding and guest execution. Do not compare
absolute times between `full` and `invalidate-full` as equivalent workloads.
No real-guest FPS or physical-latency improvement is established here.

## Validation and tracing limits

All 12 default checks and all 12 AddressSanitizer/UndefinedBehaviorSanitizer
checks passed. Explicit pixel comparisons passed at fullscreen 4K Vulkan,
fullscreen OpenGL/Cairo, fractional Vulkan, and actual compositor scale 2.
The scale-2 test observed 1280×800 logical pixels on a 2560×1600 monitor.
The sanitizer lifecycle probe also released all tracked textures. Leak detection
is disabled for GUI sanitizer runs; allocator conclusions use native processes.
See [validation.json](validation.json) for commands, renderer/scale observations,
errors and process-cleanup records.

DRM descriptors identified i915 and showed advancing render-engine counters.
These are activity counters, not completion latency. Host policy denied `perf
record`; no security settings were changed. One Callgrind attempt attributed
its captured instructions only to the scalar synthetic producer. A renderer-only
retry failed before drawing with Valgrind's `brk segment overflow`. These attempts
provide no usable internal GTK hotspot attribution. The controlled A/B trials
localize the added cost to the compositing path, but detailed CPU/GPU traces
remain open. All owned test processes were cleaned up.

## Presentation feedback follow-up

The explicit display and real-console fixtures now retain GDK timing objects
for measured updates and serialize feedback after sampling. This does not
drive frames or add observers to Pirwa. Paint samples keep their existing
format; `frame_feedback` is a separate object with named columns.
The recorder retains references so GDK's bounded frame history cannot erase
early samples during a long trial, and releases them when the trial resets or
the fixture exits.

Statistics accept only complete records with result `presented`, a nonzero
presentation timestamp and a timestamp at or after before-paint. Other results
and zeros remain raw observations, with their counts reported separately. No
predicted presentation time is substituted. On GTK versions before 4.24, result
classification is `unknown` and these statistics are unavailable; the app's
GTK 4.16 minimum is unchanged.

A protocol diagnostic on GNOME Shell 51.1 confirmed `wp_presentation` and its
`presented` events. The 1280×800 probe recorded 29 completed presentations and
one outstanding final update, and passed its image comparison. A separate
address/undefined sanitizer probe also passed. Protocol logging is diagnostic
only and is disabled for paired timing runs. These are virtual-monitor
presentation observations; physical scanout, GPU execution time and
guest-to-screen latency remain unmeasured.

The paired 4K follow-up uses the same gradient/full-invalidation workload as
above plus small updates, with two alternating pairs per workload and 60
measured updates per trial. Its frozen controls share the new recorder, packed
copy helper, compiler flags and dependencies. Per-trial host memory/I/O/CPU
pressure accompanies the raw samples. Results are retained in
[presentation.jsonl](presentation.jsonl), with derived statistics in
[presentation-summary.json](presentation-summary.json), frozen inputs in
[presentation-manifest.json](presentation-manifest.json), and correctness and
cleanup evidence in [presentation-validation.json](presentation-validation.json).

| 4K workload | Masked before-paint → virtual presentation p95 | Direct clip p95 | Masked / direct paint p95 |
| --- | ---: | ---: | ---: |
| Full invalidation | 50.479 ms | 33.504 ms | 42.630 / 27.277 ms |
| Small region | 17.013 ms | 17.099 ms | 1.386 / 1.786 ms |

Each workload/variant group contains 120 measured updates, 118 complete
presentations and two outstanding final updates. Across all eight trials,
472 presentations are accepted and eight outstanding records are retained.
No missing timestamp is treated as zero latency. Dense presentation p95 falls
33.6% in this virtual-output workload. Small-update presentation stays near one
60 Hz interval, while its paint p95 rises by 0.400 ms in this limited sample.
Further quiet-host pairs are needed before accepting a general sparse CPU/paint
regression conclusion. The host had limited free memory; pressure snapshots
surround each complete fixture process, including its post-sampling image
checks, rather than precisely delimiting the timing window. These observations
do not pass a physical-presentation release gate.

The real-console comparison is prepared separately with the same masked/direct
control and matching recorder. It requires a 3072 MiB disposable guest plus
1024 MiB available host headroom inside the private desktop. Its preflight and
existing-domain verification are retained in
[real-console-preflight.json](real-console-preflight.json). No comparison is
accepted unless the guest measurements and cleanup finish successfully. Earlier
real-console measurements above remain separate evidence.

The attempted real-console run stopped at preflight: 2368 MiB available against
4096 MiB required inside the private desktop. No guest output, domain, volume or
pool was created. Existing machines' states and inactive-definition hashes
matched before/after, and the private desktop had no remaining processes.
Real-guest validation is deferred until sufficient host memory is available.
The failed preflight is not a successful real-console benchmark.

A [later retry](real-console-followup.json) on 2026-11-15 passed memory
preflight with 4284 MiB available and saved all twelve trials. The existing-domain
baseline changed during measurement: a running machine disappeared from the
session inventory. The runner rejected the comparison. Its disposable domain
and pool were removed, with no pool files remaining. The outer launcher exited
with status 143 while the isolated inner runner continued; after it completed,
the private desktop was explicitly reaped and its runtime removed, leaving no
fixture processes. [Raw rejected trials](real-console-followup-results.json.gz)
and [frozen runner sources](real-console-followup-sources.tar.gz) are retained.
These measurements provide no accepted real-guest performance conclusion for
the rounded renderer. Earlier accepted packed-copy results remain separate.

The prepared local comparison can be resumed in fresh output directories when
memory is available and the host can remain stable during the run:

```sh
PIRWA_RUN_KVM_TESTS=1 python3 scripts/test-gnome.py \
  --monitor 3840x2160@60 --command-timeout 1200 \
  --output build/qa/rounded-real-desktop-next -- \
  python3 scripts/profile-console.py \
  --uri 'qemu+unix:///session?socket=/run/user/1000/libvirt/virtqemud-sock' \
  --iso /home/stone/Downloads/Fedora-Workstation-Live-43-1.6.x86_64.iso \
  --build build/qa/rounded-real/frozen \
  --baseline-binary build/qa/rounded-real/frozen/baseline-console \
  --expect-size 3840x2160 --modes full,small --runs 3 --seconds 10 \
  --memory-mb 3072 --setup-seconds 60 \
  --output build/qa/rounded-real-guest-next
```

These paths identify this host's retained binaries, ISO and session socket.
For another checkout/host, rebuild and freeze both variants with matching
profiling instrumentation and adjust the paths. Prepared real-console source
hashes and exact baseline compile/link commands are included in the preflight
record. Synthetic and prepared console sources, plus the measured wrapper,
are archived in [presentation-sources.tar.gz](presentation-sources.tar.gz).
The [protocol excerpt](presentation-protocol.log) is diagnostic evidence only.

GTK references: [presentation time](https://docs.gtk.org/gdk4/method.FrameTimings.get_presentation_time.html),
[timing completion](https://docs.gtk.org/gdk4/method.FrameTimings.get_complete.html),
and [frame result](https://docs.gtk.org/gdk4/enum.FrameResult.html).

## Reproduce

With the documented development dependencies and release configuration:

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf benchmark-display benchmark-display-memory
python3 scripts/profile-renderer-memory.py --prepare-only \
  --output build/qa/renderer-memory-new
python3 scripts/test-gnome.py --monitor 3840x2160@60 --command-timeout 1200 \
  --output build/qa/renderer-memory-new-desktop -- \
  python3 scripts/profile-renderer-memory.py \
  --output build/qa/renderer-memory-new --runs 2 --cycles 3
```

Prepare compiles and freezes the controls before measurement; the measurement
step refuses a non-isolated desktop or changed binaries. On the current source,
the rounded variant includes the new guarded path. The historical masked source
used here is retained with allocator XML and DRM records in
[diagnostic-sources.tar.gz](diagnostic-sources.tar.gz); its `frozen/source/`
directory predates the optimization, and `optimized/source/` contains both
`display-mask.c` and `display-direct.c` with their common fixture. Corresponding
hashes and compile flags are in the manifests. The `benchmark-display-memory`
target is available only where the required glibc functions exist.

For a new renderer-only comparison, run `scripts/profile-display.py` in the same
private desktop with `--baseline-binary`, `--sizes 3840x2160`,
`--modes invalidate-full --fullscreen --frames 60 --runs 3 --idle-seconds 0`.
Freeze both binaries before starting and retain their hashes. Keep the host
quiet and do not compile or run other profilers during accepted timing trials.
