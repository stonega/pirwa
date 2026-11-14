# Console renderer performance — 2026-11-14

This run profiles the production `PirwaDisplay` widget with synthetic xRGB
framebuffers in an isolated GNOME Wayland desktop. It does not connect to libvirt,
create a VM, decode SPICE, or establish physical-display presentation latency.

## Change

The console now delegates rendering to `src/pirwa-display.c`. The transport still
owns primary memory and clears the widget before freeing it. GTK snapshots own
immutable pixel data. A newly announced primary is validated immediately and
copied when first drawn, avoiding an extra eager full-frame copy.

For opaque RGB, small updates replace 256×256 tiles over a retained base texture.
At most 32 replacement tiles are cached. More than a quarter of the frame covered
by replacements, dense updates, ARGB, Cairo, or an extreme downscale take the
full-frame path. Minified sources whose dimensions do not align with the sampling
grid also use full frames, preserving the full image's mipmap sampling phase.
Compaction replaces the base and releases the patches. Sampling padding is
bounded to 32 source pixels; the retained patch payload is at most 12.5 MiB,
excluding the base, transport memory, GTK caches and in-flight render resources.

Clips align with device pixels. Each patch includes its bilinear sampling
footprint; changes near a boundary also invalidate neighboring sampling data.
Patch draws replace old edge coverage over black, preventing dark seams or stale
pixels at fractional scaling. Sparse frames share one source-pixel transform so
tile sampling stays aligned at HiDPI. Dense frames retain the original direct
texture path. Both paths use GTK's default texture sampling.
Transparent ARGB uses the whole-frame path to preserve alpha composition.

Damage regions are coalesced, with fragmented regions reduced to their extents
after 64 rectangles. There is no continuous tick or sampling timer in the app.
The benchmark drives updates explicitly; its passive idle phase removes that tick.

## Method

`tests/benchmark-display.c` links the same display/frame source as Pirwa. It uses
30 warmup frames, then records each measured GTK frame. Workloads are:

- `full`: regenerate and invalidate the whole source framebuffer.
- `small`: change a fixed 64×64 region.
- `burst`: the small region plus 15 overlapping writes/invalidations per update.

The comparison uses four paired trials of 60 measured frames for every workload
at 1280×720, 1920×1080 and 3840×2160. Pair order alternates and case order rotates.
The baseline binary preserves the initial extracted renderer, whose full-copy
and default texture rendering match the original console implementation. Both
versions use the same workload and installed GTK libraries.
The saved baseline executable predates the exhaustive image comparison and
actual-scale assertions. Those later QA checks run outside measured phases;
all 36 current-variant trials include the full-image comparison after timing.

`paint_wall_us` and `paint_cpu_us` span before-paint through after-paint, including
the Update phase's synthetic pixel generation. `producer_cpu_us` records that
main-thread generation separately. CPU percentages cover all process threads
and include the producer. These are app update/submission measurements; they
exclude compositor presentation and GPU completion. Cadence is completed GTK
updates per elapsed second, not physical monitor FPS.

Raw data, binary hashes, source hashes, dependency versions, power profile,
affinity and host load are in `build/qa/display-final-profile/results.json`.
The original matrix and saved baseline binary are in `build/qa/display-baseline/`.
All are ignored build artifacts. PMU stack sampling was unavailable under the
host's perf access policy; no system security settings were changed.
The reference host is an Intel Core Ultra 9 185H running Fedora Workstation 45
with the balanced platform profile. Exact kernel and library versions are in
the archived environment record.

## Paired results

Each cell shows baseline → current. CPU is the median of four trials; frame p95
pools 240 measured frames per variant. The source is scaled into a 960×601
logical-pixel viewport on a 1280×800, 60 Hz virtual monitor (scale 1,
GskVulkanRenderer).

| Source / change | CPU, % of one core | Frame-clock span p95, ms | GTK updates/s |
| --- | ---: | ---: | ---: |
| 1280×720 / full | 34.44 → 35.63 | 7.33 → 7.45 | 60.0 → 59.9 |
| 1280×720 / small | 26.08 → 5.59 | 5.70 → 1.37 | 59.9 → 60.0 |
| 1280×720 / burst | 26.58 → 5.64 | 5.61 → 1.37 | 60.0 → 60.0 |
| 1920×1080 / full | 65.51 → 64.03 | 13.71 → 12.87 | 60.1 → 60.0 |
| 1920×1080 / small | 47.66 → 8.21 | 10.80 → 1.84 | 59.5 → 60.0 |
| 1920×1080 / burst | 50.10 → 7.49 | 10.42 → 1.53 | 60.0 → 60.0 |
| 3840×2160 / full | 87.72 → 87.61 | 53.84 → 52.08 | 21.2 → 21.5 |
| 3840×2160 / small | 85.36 → 7.63 | 37.67 → 1.66 | 27.4 → 60.0 |
| 3840×2160 / burst | 85.95 → 7.24 | 37.45 → 1.58 | 28.1 → 60.0 |

The sparse path improves this synthetic workload. Full-frame 4K remains below 60 updates/s. These results do not establish real-guest or physical-display FPS.

The ten-second passive idle samples recorded current: 0.0129% of one core, 0 paint callbacks, baseline: 0.0019% of one core, 0 paint callbacks.

All raw frame samples and environment details are retained in
[paired-results.jsonl](paired-results.jsonl). Each run stores arrays in the order
given by `sample_columns` in the environment record. Source hashes identify the
production renderer and benchmark used for the measured executable.

## Reproduction

Build optimized code with symbols, then run in the virtual desktop:

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf benchmark-display
./scripts/test-gnome.py --command-timeout 1000 --output build/qa/display-profile -- \
  env GTK_A11Y=test ./scripts/profile-display.py \
    --runs 4 --frames 60 --output build/qa/display-profile/results.json
```

For alternating before/after trials, save the benchmark executable before changing
the renderer, then pass `--baseline-binary /path/to/saved/benchmark-display`.
Use a fresh output path. The wrapper writes each result as it completes, preserves
all frame samples, and produces summaries per resolution/workload/variant.
`--sizes 1920x1080` and `--idle-seconds 0` provide a shorter investigation.

The GUI executable also compares sparse output against freshly copied full frames
pixel by pixel (at most one 8-bit channel level of rounding tolerance). It exercises fractional
scaling, tile borders, repeated/disjoint/fragmented changes, compaction, ARGB alpha,
clear/free, invalid primaries and primary recreation. These checks run after timing.

For a correctness run without the performance matrix:

```sh
./scripts/test-gnome.py --output build/qa/display-check -- \
  env GTK_A11Y=test ./build/tests/benchmark-display \
    --width 1234 --height 777 --frames 30 --mode burst
```

Use the compositor's scale rather than relying on `GDK_SCALE`, which was ignored
by the Wayland surface in this environment. A larger virtual monitor permits an
actual scale-2 surface, and the benchmark asserts the observed scale:

```sh
./scripts/test-gnome.py --monitor 2560x1600@60 --output build/qa/display-hidpi -- \
  python3 scripts/qa-display-scale.py --scale 2 -- \
    env GTK_A11Y=test ASAN_OPTIONS=detect_leaks=0 ./build-sanitize/tests/benchmark-display \
      --expect-scale 2 --width 1234 --height 777 --frames 30 --mode burst
```

The scale helper requires Python GObject bindings and refuses normal desktops or
physical monitors. It changes only the private Meta-0 monitor using Mutter's
temporary configuration method. The wrapper removes the private compositor and
settings after the command. `GSK_RENDERER=cairo` or `GSK_RENDERER=gl` provides a
separate renderer check. The default Meson suite remains display-free; cropped
texture bounds, padded strides, conversion and immutable ownership have headless checks.

## Correctness validation

Observed results and exact commands are in [validation.json](validation.json).

The eleven default checks and Address/UndefinedBehaviorSanitizer checks pass with
leak detection disabled for the existing libvirt initialization limitation.
The fractional-scale comparison checks every output pixel against full snapshots.
Verified Vulkan and OpenGL cases differ by at most 1/255 per channel. Actual
scale-2 surfaces are checked under sanitizers. Cairo uses full snapshots and
matches the reference exactly; independently transformed cropped surfaces did
not preserve its sampling phase. Minified odd-sized sources retain full frames
for the same reason; aligned near-8K sources also have a pixel comparison.
Cropped ownership and stride/bounds checks are
part of the display-free default suite. Isolation/argument guards also reject
running the scale helper on the ordinary desktop.

The seven-case manager UI matrix passes, including Boxes onboarding, large dark
text, and the embedded/detached viewer lifecycle. Its viewer fixture can supply
virtual keyboard input only inside the private desktop, waiting for compositor
visibility before checking usage data. This uses Mutter's private
[RemoteDesktop test interface](https://raw.githubusercontent.com/GNOME/mutter/51.1/data/dbus-interfaces/org.gnome.Mutter.RemoteDesktop.xml)
and adds no input automation or polling to production code. Final desktop cleanup
reports contain no remaining private-session processes.

## Remaining gates

Full-screen 4K updates still copy and upload a complete frame. Sparse-update
results cannot establish video, SPICE decoding, guest execution, accelerated
scanout, physical input/audio or presentation performance. VNC and SPICE GL still
use the external viewer. Broader host/GPU and real-guest active-frame profiling
remain release work.
