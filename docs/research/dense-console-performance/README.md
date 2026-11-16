# Dense console follow-up — 2026-11-14

Packed RGB frames now use one `memcpy` for the immutable CPU snapshot in
`pirwa_frame_texture()`. Padded rows and 16-bit conversion use the row loop.
The tile budget, sampling, texture ownership and collection idle work are unchanged.
This adds no dependency, retained buffer cache or production instrumentation.

## Copy measurement

The headless `benchmark-frame` executable times production texture creation,
including allocation, copying and the GTK memory-texture builder. It initializes
a static packed xRGB source once, retains three textures, warms for 30 copies,
then records 180 creation samples. Releasing old textures is outside timing.
There is no display, SPICE decoding, guest, pixel producer or GPU work.

Both executables were compiled with the same `cc -O3 -std=c17` command and
libraries. The baseline uses `src/pirwa-frame.c` from `f332a02`; the candidate
changes the packed-copy branch only. There are three alternating pairs per size,
540 samples per variant/size. The table pools samples and uses nearest-rank p95.
These final copy trials ran after the GUI profiler and compilation had finished.
An earlier overlapping run is excluded.

| Source | Median creation CPU, ms | Creation CPU p95, ms |
| --- | ---: | ---: |
| 1920×1080 | 0.919 → 0.858 | 1.089 → 0.963 |
| 3840×2160 | 4.544 → 2.062 | 5.042 → 2.723 |

Arrows show baseline → packed-copy candidate. The 4K median decreases about 55%.
These are copy-component results, not application FPS or presented-frame latency.
The host is Fedora 45 on a Core Ultra 9 185H. Exact packages, source/binary hashes,
host pressure and every sample are in [packed-copy.jsonl](packed-copy.jsonl).

## Real-console comparison

Twelve ten-second trials completed: three alternating pairs each for dense and
small updates, using fresh native clients in one disposable Fedora live guest.
Both variants used four vCPUs, **3072 MiB RAM**, a 16 GiB disk, software SPICE,
3840×2160 source/viewport, scale 1 and `GskVulkanRenderer`. The smaller QA memory
setting was selected after the earlier setup stalls; it does not change app
defaults. These results are separate from the historical 4096 MiB
dataset. The observed host power profile was balanced; audio was disabled.

**Scope:** the binaries were pinned before concurrent rounded-display edits.
Both use the original display renderer from `f332a02`; the production difference
is the packed-copy change. The new rounded renderer and window edits are outside
this comparison and need their own measurements.

Arrows show baseline → packed-copy candidate. CPU and update cadence are medians
of three trials; paint p95 pools all frames and uses nearest rank.

| Workload | Client CPU, % of one core | Copy CPU, % of one core | GTK updates/s | GTK paint span p95, ms |
| --- | ---: | ---: | ---: | ---: |
| Dense | 88.62 → 88.84 | 14.03 → 9.70 | 41.14 → 43.37 | 27.755 → 25.733 |
| Small | 6.63 → 6.65 | 0.47 → 0.46 | 57.79 → 57.80 | 1.559 → 1.555 |

The dense full-copy creation CPU median falls **4.847 → 2.868 ms**, about 41%.
This pools copy-CPU samples from frames containing full copies and no tile
copies, divided by the full-copy count. The candidate records packed 15,360-byte
rows. Dense paint p95 decreases about 7%, while total client CPU is essentially
unchanged and more GTK updates complete. Small-update results are similar.
Guest draws remain about 16.9/s for dense and 30.0/s for small updates. GTK updates
are not guest or physical-display FPS; spans exclude GPU completion/presentation.

End-of-trial dense PSS medians are **232.3 → 273.3 MiB**. The three snapshots range
217.6–271.3 MiB for baseline and 248.3–277.9 MiB for candidate. These variable
endpoint samples do not measure steady-state idle or peak memory; allocation and
driver traces are needed to explain the difference. This is not a memory gate pass.
Observed global memory and I/O full-stall totals stayed below 0.5% of the recorded
observer intervals during accepted trials. Background work was not controlled.

All trials retained the expected source size, had zero primary changes, and
finished unsuspended. All warmups met the five-second quiet window and 30-second
deadline. The fixture, volume and pool were removed; existing-domain hashes/states
matched, and the private desktop has no remaining processes. Raw data and source
scope are in [real-console.jsonl](real-console.jsonl); per-trial metrics are in
[real-console-summary.json](real-console-summary.json).

Before this run, a [partial comparison](rejected-frame-change.jsonl) was rejected
after a primary surface was recreated during measurement. A subsequent
[setup attempt](setup-pressure-attempt.json) timed out under host pressure, as did
the [earlier attempt](real-console-attempt.json). Those attempts are excluded.
The runner now waits for the expected framebuffer and agent state to stay quiet
for five seconds before sampling. Its deadline also rejects a host clock stall;
five headless regression checks cover those conditions. In-trial changes still
fail validation.

Dense submission p95 still exceeds the 16.7 ms interval of a 60 Hz display. The
[rounding and memory follow-up](../renderer-memory/README.md) investigates
allocator retention and the later rounded renderer, with a separate synthetic
comparison. Detailed CPU/GPU traces and broader guest/presentation workloads
remain open; this record's renderer and real-console results are unchanged.

## Experiments rejected

The following experiments are outside the shipped implementation. Their raw
records retain the experimental variant as `current`.

- **Eight cached tiles:** one initial fullscreen 4K band pair increased CPU from
  54.55% to 84.42% of one core. The investigation stopped after that pair and one
  unpaired small-update trial. The 32-tile budget remains.
- **Reuse hints across compaction:** three real-guest pairs measured median
  dense CPU 90.40% → 89.75%, but pooled paint p95 28.147 → 29.183 ms. There was
  no reliable tail improvement. The original compaction behavior remains.
- **One released CPU buffer cached per display, capped at 32 MiB:** three dense
  real-guest pairs did not reduce copy CPU or reliably improve paint latency.
  The cache and its lifecycle code were removed.

Raw records: [small cache](rejected-small-cache.jsonl),
[compaction hints](rejected-compaction-hints.jsonl),
[buffer cache](rejected-buffer-cache.jsonl). These are exploratory results on one
host, not general comparisons of the techniques. Early setup attempts also
failed while the host had memory/I/O pressure or Fedora's welcome screen was
initializing. They produced no accepted samples; their disposable resources
were removed and the existing-domain baselines matched.

## Reproduce

With the normal development dependencies installed:

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf benchmark-frame benchmark-console qa-vm
build-perf/tests/benchmark-frame 3840 2160 180
```

For the same headless before/after comparison, save the old source and compile
both copies with identical flags and the current benchmark fixture:

```sh
mkdir -p build/qa/copy-reference
git show f332a02:src/pirwa-frame.c > build/qa/copy-reference/frame.c
cc -O3 -std=c17 -Isrc tests/benchmark-frame.c build/qa/copy-reference/frame.c \
  $(pkg-config --cflags --libs gtk4 spice-client-glib-2.0) \
  -o build/qa/copy-reference/baseline
cc -O3 -std=c17 -Isrc tests/benchmark-frame.c src/pirwa-frame.c \
  $(pkg-config --cflags --libs gtk4 spice-client-glib-2.0) \
  -o build/qa/copy-reference/current
```

Run three pairs of `baseline 3840 2160 180` and `current 3840 2160 180`, alternating
order, with no other profiling or compilation from this workflow in progress.
Repeat with `1920 1080 180`. Record host pressure and background load.

For real consoles, follow the [disposable-guest workflow](../active-console-performance/README.md)
with `--baseline-binary`, `--modes full,small`, `--runs 3 --seconds 10`, a
3840×2160 virtual monitor, `--memory-mb 3072` and `--setup-seconds 60`.
Both paths use native tile
rendering in fresh client processes. Audio and the collection UI are excluded.
The matching baseline instrumentation can be recreated in a separate worktree
at `f332a02` with `git apply --unidiff-zero baseline-instrumentation.patch`;
use this directory's absolute patch path. Its
[source manifest](baseline-manifest.json) identifies the saved baseline.
Run `python3 tests/test-console-profile.py` for the five readiness/deadline
regressions, without creating a guest or opening a display.

The pixel comparison, fractional/HiDPI, sanitizer and cleanup records are in
[validation.json](validation.json). Physical presentation, accelerated scanout
and broader guest applications remain separate release gates.

## Correctness checks

All 12 normal and 12 address/undefined sanitizer checks passed. The frame-only
suite also passed with leak detection enabled; the full sanitizer run disables
leak detection for the previously recorded libvirt initialization finding.
New headless tests compare packed and padded RGB/ARGB snapshots and retain a
texture after its source memory changes.

Seven programmed manager UI cases passed in private GNOME. Fullscreen 4K
comparisons had zero channel error; fractional Vulkan/OpenGL and actual scale-2
Vulkan comparisons stayed within the existing one-level tolerance. Cairo had
zero error. These image checks cover fragmented/disjoint damage, tile borders,
compaction, ARGB, clearing and primary replacement. They do not certify physical
input or presentation. The fullscreen GUI timing trials overlapped initial
copy profiling/compilation, so only their correctness results are accepted here.
