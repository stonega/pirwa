# Startup and collection presentation

On 2026-11-16, the production executable completed 30 warm launches on both
an isolated virtual GNOME desktop and the physical Wayland display. Warm startup
still exceeds the 500 ms target. The virtual collection runs also leave frame
time and default-renderer memory acceptance open. No performance budget or
production renderer policy changed.

The host was Fedora 45, Linux 7.2.15, Core Ultra 9 185H, balanced power profile,
GTK 4.24.1, libadwaita 1.10.0 and libvirt 12.6.0. The virtual monitor was
1280×800 at 60 Hz. Mutter reported the physical eDP-1 display at 2560×1600,
240.0003 Hz and scale 1. Display settings were left unchanged. Raw profiles
retain package versions, load at start/end, renderer, scale and binary hashes.
These are single-host observations, not a claim about all supported systems.

| Mode | Domains | Measured warm launches | After-paint p95 | First presentation p95 |
| --- | ---: | ---: | ---: | ---: |
| Shared window/backend, virtual, default Vulkan | 100 | 30 | 569.175 ms | 617.762 ms |
| Shared window/backend, virtual, default Vulkan | 1,000 | 30 | 619.108 ms | 669.561 ms |
| Production entry point, virtual, default Vulkan | 100 | 30 | 551.132 ms | 591.957 ms |
| Production entry point, physical 240 Hz, default Vulkan | 100 | 30 | 586.293 ms | 640.490 ms |
| Explicit GL diagnostic control, virtual | 100 | 30 | 497.765 ms | 537.644 ms |

Each run discarded one warm-up from the percentile. Presentation uses only
complete `presented` GDK results with a positive monotonic timestamp. After-paint
is submission; it does not mean the window has appeared. The production mode
uses the unchanged executable and its unique GApplication lifecycle with a
QA-only preload observer. It does not measure desktop-launcher activation,
an existing application instance or cold caches. Synthetic libvirt domains
are local to each process and do not establish real-daemon inventory latency.

At 100 domains, the default Vulkan run sampled 60.030 seconds of idle time:
0.0030% CPU of one core, zero after-paint frames and 104,289 KiB PSS
(101.8 MiB). CPU meets its target in this sample; PSS exceeds 100 MiB.
The explicit GL control sampled 60.028 seconds, 0.0021% CPU, zero frames and
91,360 KiB PSS (89.2 MiB). These are separate trials, not a paired renderer
acceptance study. They do not justify a default renderer change or certify
hidden-window work, console performance or accessibility.

The virtual 1,000-domain five-second scroll recorded 285 complete presentations
and one outstanding result. Before-paint to presentation p95 was 19.385 ms;
presentation interval p95 was 19.506 ms, with 123 intervals over 16.7 ms.
Its paint wall-time p95 was 17.939 ms. A later instrumentation check recorded
wall time 16.514 ms and main-thread CPU 16.362 ms separately; its presentation
latency was 17.704 ms and interval 17.474 ms (286 presented, one outstanding).
The latter is a separate five-second trial, not an application improvement.
Historical `cpu_frame_work_p95_ms` values were wall-clock spans despite their
name; new reports label wall time and thread CPU explicitly.

Five diagnostic startup launches measured median GTK init 24.318 ms, Adwaita
init 68.268 ms, registration 10.378 ms, window construction 7.880 ms and
`gtk_window_present` 211.078 ms. The explicit GL control's presentation call
median was 87.999 ms. This points to toolkit/render initialization as an area
to trace; it does not establish a particular optimization or a backend stall.

The hardware 1,000-domain run stopped after 25 launches. The next mapped window
was inactive and supplied only discarded/skipped feedback before the watchdog.
The screen was unlocked when checked afterward; occlusion/focus is a possible
cause, not an established diagnosis. That incomplete run is retained without
a 30-launch percentile or scrolling conclusion. An earlier locked-screen attempt
also supplied no presented feedback and was rejected. Physical scrolling/idle
and the 60 Hz hardware reference remain open.

All runs used a private D-Bus and private runtime/config/data/cache/state,
in-memory settings and matching pinned executable/schema. `GTK_A11Y=test` was
explicit because the private session could not activate the host AT-SPI service.
This does not certify screen-reader use. The physical wrapper creates no
compositor and only cleans up processes carrying its own private runtime marker.
Every completed and failed run retained here finished with no fixture processes.

Reproduce the production virtual run from a normal development build:

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf -j 2
./scripts/test-gnome.py --output build/qa/application-startup -- \
  env GTK_A11Y=test ./scripts/profile-ui.py --build build-perf --application \
  --domains 100 --launches 30 --idle-seconds 0 --display-kind virtual \
  --output build/qa/application-startup/profile.json
```

For the hardware startup run, add `--physical` to the wrapper and use
`--display-kind physical`. For collection/idle work, omit `--application`,
add `--presentation` and request `--idle-seconds 60` or
`--domains 1000 --idle-seconds 0 --scroll-seconds 5`. Leave hardware windows
unobscured throughout the run. The [performance contract](../../design/performance.md)
describes the remaining cold-cache, real-daemon, Sysprof and guest-throughput gates.

[Validation](validation.json) contains the result table and artifact hashes.
[Compressed raw results](raw-results.json.gz) preserve all eight profile reports,
desktop commands/outcomes and process cleanup; read with Python's `gzip.open`
and `json.load`. [Provenance](provenance.json), the baseline source archive and
the instrumentation source archive distinguish the revisions used during this
investigation. Production sources did not change across these performance runs;
QA instrumentation was extended between trials. This is an uncommitted development
candidate, not release sign-off. Regression-check outcomes are recorded in the
validation file: 19 headless checks and 18 virtual GNOME UI cases passed in each
of the normal and ASan/UBSan builds. Leak detection remained disabled, so lifecycle
leak acceptance stays open. The first sanitizer test attempt omitted the SDK's
runtime loader path; three cases failed before loading ASan. With the matching
SDK libraries available, all 19 passed. That setup failure is preserved in
provenance and is not treated as a sanitizer finding.

The [frozen build-source archive](candidate-build-source.tar.gz) contains the
source, schemas, Meson files and scripts used for the sanitizer rebuild; its
per-file manifest is in provenance. These host diagnostic builds used extracted
SDK include/library paths. The separate [clean native packaging record](../clean-native-build/README.md)
remains the evidence for normal Fedora RPM dependency/build/install checks.
