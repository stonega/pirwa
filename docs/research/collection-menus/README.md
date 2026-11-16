# Collection menus and startup follow-up

On 2026-11-16, Pirwa stopped constructing an options popover for every bound VM
row. Each row now keeps its UUID and uses GTK's
[create-popup callback](https://docs.gtk.org/gtk4/method.MenuButton.set_create_popup_func.html)
to construct Settings, Remove and Force Stop when opened. Unbinding closes and
discards the old model, popover and UUID. Selection and stale-target checks are
unchanged. No renderer policy, VM definition, dependency or performance budget
changed.

## Measurements

Both frozen executables use the optimized release build, the same host libraries
and default Vulkan renderer. Tests use synthetic libvirt definitions, memory
GSettings, a private D-Bus and private XDG directories in virtual GNOME at
1280×800, 60 Hz, scale 1. The host is Fedora 45, Core Ultra 9 185H, Linux 7.2.15,
GTK 4.24.1 and libadwaita 1.10.0, with the balanced power profile. Each raw report
records load, packages, graphics environment and executable/schema hashes.
`GTK_A11Y=test` is explicit; these runs do not certify AT-SPI or physical input.

The alternating order was before, after, after, before, before, after in one
private desktop. Each production-startup trial discarded one warm-up and kept
ten launches: thirty measured launches per executable across three trials.
Percentiles below use the combined raw launch samples, not averages of trial
percentiles. Separate scroll trials used 1,000 domains and five seconds of
continuous scrolling per trial; startup samples from those short scroll trials
are not used for launch acceptance.

| Alternating comparison | Before | After |
| --- | ---: | ---: |
| Production startup, 100 domains, after-paint p95 | 663.999 ms | 562.260 ms |
| Production startup, 100 domains, first presented p95 | 707.447 ms | 607.657 ms |
| Scroll paint CPU p95, three trials | 19.483 / 20.851 / 21.341 ms | 5.524 / 5.500 / 5.147 ms |
| Scroll paint wall time p95, three trials | 19.666 / 21.090 / 21.561 ms | 5.616 / 5.590 / 5.190 ms |
| Scroll presentation interval p95, three trials | 21.740 / 23.314 / 24.067 ms | 17.370 / 17.363 / 17.366 ms |

The scroll improvement repeats in every trial. Each after trial contains
298–299 complete presented results and one outstanding result; all result counts
remain in the raw profiles. Paint wall time and thread CPU exclude compositor
presentation and GPU completion. The virtual presentation interval still exceeds
16.7 ms and does not establish the physical-display gate.

Standalone trials, each with one discarded warm-up and thirty measured launches,
show the variation between sessions:

| Standalone startup comparison | Before first presented p95 | After first presented p95 |
| --- | ---: | ---: |
| Production entry point, 100 domains | 643.345 ms | 634.942 ms |
| Shared window/backend QA, 100 domains | 701.756 ms | 612.330 ms |
| Shared window/backend QA, 1,000 domains | 764.089 ms | 591.906 ms |

Do not promise the alternating comparison's 100 ms tail reduction on every
launch. Both production comparisons still miss the 500 ms warm-start target.
Cold launch and hardware presentation remain open.

The standalone 100-domain runs also sampled sixty seconds of idle collection
work. Before/after PSS was 60,041 / 49,024 KiB, CPU was 0.0195% / 0.0159% of one
core, and both recorded zero after-paint frames. The PSS difference is 10.8 MiB
in these two samples. It is not a paired memory acceptance study or a general
renderer comparison; keep it separate from older host sessions and their memory
findings.

## Investigation and controls

Debugger samples captured startup paths in three default-Vulkan and three
explicit-GL diagnostic launches. Warm default samples include Vulkan instance
preparation and dynamic driver loading; initial runs also include pipeline and
shader work. Application samples exposed eager row-menu construction. Debugger
interrupts perturb execution, so their wall times and sample counts are not CPU
percentages or launch percentiles. They do not certify the absence of backend
stalls. No production instrumentation or timers were added.

The host permits `perf stat` counters but denied `perf record` call-stack
sampling. An attempted old `SYSPROF_TRACE_FD` capture was empty: installed GTK
uses the [Sysprof collector](https://github.com/GNOME/gtk/blob/4.24.1/gdk/gdkprofiler.c),
whose [current transport](https://github.com/GNOME/sysprof/blob/51.0/src/libsysprof-capture/sysprof-collector.c)
uses `SYSPROF_CONTROL_FD`. The installed Sysprof agent then failed because
`org.gnome.sysprof3.profile` is not registered. These setup failures are retained;
host permissions and system configuration were left unchanged. An initial
debugger sampling attempt hit its sample limit during teardown; the completed
runs resume the process through normal exit and retain the partial attempt too.

## Correctness and reproduction

The desktop action test now opens a previously unbuilt menu, filters its row
out while retaining the button, requires the old popover to be discarded, then
restores the first VM before exercising Settings/Remove/Force Stop. A separate
100-domain run also filters to another VM and checks every menu target before
restoring the original. This covers UUID reuse without operating real guests.
Normal and narrow 200% text/dark variants are exercised.
Both ten-case desktop matrices and all four 100-domain action cases pass, along
with twenty normal and twenty leak-enabled sanitizer headless checks. The first
sanitizer desktop attempt caught a test synchronization error: a changed model
does not mean GTK has already unbound its widgets. The corrected test observes
the factory's actual unbind signal before inspecting the discarded popup; no
sleep, longer watchdog or production workaround was added. The failed attempt
is retained separately from the passing final runs.
A final offline rootless Fedora check builds the corrected sources against
normal RPM dependencies, passes twenty headless checks in each build, and
builds, installs and verifies the RPM. The local developer build was rebuilt too.

Build the release candidate and use the existing profiler:

```sh
meson setup build-perf --buildtype=release -Db_ndebug=true
meson compile -C build-perf
./scripts/test-gnome.py --output build/qa/collection-profile-new -- \
  env GTK_A11Y=test ./scripts/profile-ui.py --build build-perf \
  --domains 1000 --launches 30 --idle-seconds 0 --scroll-seconds 5 \
  --presentation --display-kind virtual \
  --output build/qa/collection-profile-new/profile.json
```

Use normally installed development packages, or the documented local SDK
workaround on this host. See [the performance workflow](../../design/performance.md)
for production-entry-point measurements and physical-display restrictions.
The before/after sources, frozen binary hashes, exact diagnostic commands,
failed attempts and complete profiles are retained in [validation](validation.json),
[compressed raw results](raw-results.json.gz) and the [source archive](sources.tar.gz).
The working tree remains uncommitted; this is development evidence, not a release
sign-off. Existing toolkit heap findings, historical readiness failures, real-host
acceptance, metadata and final source review remain separate work.
