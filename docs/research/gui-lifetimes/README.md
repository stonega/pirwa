# GUI object lifetimes and dependency leak reports

The 2026-11-16 follow-up checks Pirwa object finalization separately from the
process-exit heap reports in the [earlier investigation](../lifecycle-leaks/README.md).
Thirty-two cycles pass in both a normal build and ASan/UBSan with an explicit
Intel Vulkan driver. Each run observes 640 objects, all finalized. Full GUI
LeakSanitizer acceptance remains open because the installed toolkit and font
stack still report allocations. The repeated checks also exposed a Settings
opening race, now fixed and covered by deterministic regression checks.

## Settings opening race

A leak-enabled stress run stopped on cycle 29 waiting for Settings to appear.
An inventory event can replace the selected VM's pending details request. The
old request carried the user's instruction to open Settings; the background
replacement only loaded resources, so the click was lost when the old result
was discarded by its generation fence.

The window now retains the explicit request for that selected UUID across
same-VM refreshes. Changing or clearing selection, switching connections,
closing the window, and completing the current details request clear it.
Existing backend and details-generation fences still reject obsolete replies.

The fixture injects a real test-driver inventory snapshot during a Settings
details completion. The unchanged pre-fix code fails deterministically on the
first cycle; the fixed code opens Settings. A separate injection clears
selection during another Settings request, then reselects the VM and verifies
that no stale dialog appears. Each final 32-cycle run passes 16 refresh and 16
selection-invalidation checks, normally and under ASan/UBSan. These checks do
not independently certify switching connections to the same UUID.

This identifies the newly observed Settings failure. The older startup and
Boxes step-4 readiness failures still have no proven cause.

## Ownership regression

`tests/test-ui-lifetime.c` uses the production window, backend, console and
dialogs with a private libvirt test-driver XML file. Each cycle opens and closes
Preferences and creation. Alternating cycles then exercise stopped-guest
Settings or keep a detached console alive after closing the collection and
finally close that console. No existing guest or real libvirt daemon is used.

Test-only wrappers observe Pirwa constructors, settings and SPICE sessions,
channels and audio helpers. The collection's selection, filter and store are
also observed. A qdata destruction marker counts finalization without retaining
the object. This matters because ordinary
[GObject weak notifications run during disposal](https://github.com/GNOME/glib/blob/main/docs/reference/gobject/concepts.md#weak-references),
which can happen while other references still exist. A control retains the first
window after it closes, verifies that its finalization marker remains, then
releases it and requires cleanup to finish.

The test joins only its own backend workers and lets pending main-loop callbacks
finish before checking every observed type. Production code has no join or
tracking hook. An ordinary 20-second watchdog detects stuck transitions; it is
not a timing budget. Fixture files live under the private desktop's runtime
directory so the runner can remove them after an abort.

The desktop matrix now contains ten cases, including an eight-cycle ownership
case. Ten normal and ten Intel-Vulkan sanitizer cases pass after the Settings
fix. A fresh offline rootless RPM check also builds, installs and verifies the
package. The sanitizer matrix
disables heap leak detection; the ownership markers and ASan/UBSan checks remain
active. Twenty headless checks pass separately with leak detection enabled and
the existing narrow libvirt initialization exception.

## Heap controls

Software Cairo rendering removes GPU allocations from these diagnostic runs.
The final fixed application ownership fixture was run with leak detection enabled:

| Window cycles | Total reported bytes | Fontconfig allocation stacks | GTK color allocation stacks |
| ---: | ---: | ---: | ---: |
| 1 | 422,576 | 422,320 | 256 |
| 8 | 424,368 | 422,320 | 2,048 |
| 32 | 430,512 | 422,320 | 8,192 |

All observed objects finalized in each run. The fixed Fontconfig component did
not grow with the cycle count. These are allocation reports, not PSS or a
performance measurement. Each process intentionally exits nonzero because of
the visible heap findings; the diagnostic runner's pass means its ownership
assertions completed and those reports were captured, not that the heap is clean.
A minimal GTK label window with no Pirwa/libvirt code
also reports 306,347 bytes in 7,038 allocations from Fontconfig. Allowing
unaligned pointers in the scanner does not change either result.

Explicit Pango/Fontconfig shutdown in a separate minimal diagnostic reduces its
report to 2,972 bytes in 72 allocations. An additional global Cairo reset aborts
on live cache entries; it is not a valid full-toolkit cleanup. Neither cleanup
call nor a broad Fontconfig/GTK suppression was added to Pirwa.

## GTK 4.24.1 accent-color leak

The growing component allocates four 64-byte GTK color values per Pirwa cycle.
A standalone GTK control that sets `gtk-accent-color` and uses `@accent_color`
reproduces one 64-byte color leak per window: 64 bytes for one cycle and 2,048
bytes for 32. Replacing that control's CSS color with the same literal RGBA value
removes the GTK color leak. Fontconfig reports remain in both controls.

In [GTK 4.24.1](https://github.com/GNOME/gtk/blob/4.24.1/gtk/gtksettings.c#L999),
the settings style provider creates a new color value on each named-accent
lookup. Its caller resolves the borrowed value without releasing that original
allocation. This identifies a dependency ownership defect rather than a retained
Pirwa window. The [upstream main implementation](https://github.com/GNOME/gtk/blob/278eeda31489417a425b7767ab191b3853f90d00/gtk/gtksettings.c#L997)
already caches the value on the settings object and releases it at finalization.
The inspected stable branch still lacks that fix. No fixed released GTK binary
was tested here, and no application CSS or accent-color policy was changed.

Before GUI leak sign-off, repeat these controls using a GTK package containing
that fix and assess the remaining Fontconfig reports independently. The raw
findings remain visible; the existing libvirt-only exception is unchanged.

## Reproduce and scope

```sh
meson setup build --reconfigure
meson compile -C build
./scripts/test-gnome.py --output build/qa/ui-lifetime-new -- \
  ./build/tests/test-ui-lifetime --cycles 32
```

On this Intel/NVIDIA host, the sanitizer ownership control is:

```sh
ASAN_OPTIONS=detect_leaks=0 \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json \
  ./scripts/test-gnome.py --build build-release-review \
  --output build/qa/ui-lifetime-sanitizer-new -- \
  ./build-release-review/tests/test-ui-lifetime --cycles 32
```

Use the extracted runtime loader path from [setup](../../implementation/setup.md)
on this development host. The driver filename is host-specific. Building the
lifetime target regenerates its compiled settings schema; this was verified by
removing that build artifact and rebuilding only the target. An earlier attempt
used an old schema missing a newly added preference and stopped before the
ownership checks; it is retained separately from the accepted runs.

This is programmed ownership testing in a non-unique QA application, not
physical input/accessibility, guest audio/frames, long-duration heap acceptance,
or production unique-instance startup certification. The historical readiness
timeouts and performance budgets remain separate gates.

[Validation](validation.json), [compressed raw results](raw-results.json.gz) and
the [source archive](sources.tar.gz) retain exact inputs, binary/schema hashes,
controls, failed diagnostics and results. Source changes from concurrent work
were preserved. Pinned binaries and matching schemas were used for the final
32-cycle comparison, with source drift checked separately.
