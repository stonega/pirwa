# Boxes UI readiness investigation

The initial harness fix on 2026-11-15 passed **14 GNOME UI cases in each of the normal
and ASan/UBSan builds** (two seven-case matrices). Each of the four Boxes runs
per build confirmed three observed/painted windows. Both builds also passed all
14 headless checks. A focused `--trace` invocation passed. Layout captures:
[normal](normal.png), [200% text/dark](large-text-dark.png).

The UI harness had a reproduced window-lifecycle defect: after replacing the
collection window, it still observed the first window's frame clock. Its
`first_paint` flag also belonged to that closed window. A new clock-identity
assertion failed against the old harness at the manual-reopen step.

The trace showed replacement windows at frame `-1` as reopen steps began,
while the harness still retained the closed window's paint state. There was no
current-window paint condition before manual activation or final completion. These findings concern the test harness;
no application dialog-presentation change was made.

## Fix and regression

The harness now disconnects the old map/frame observers before replacing a
window, attaches to the replacement's frame clock and tracks its first paint.
Manual import activation and the final reopen check wait for that window to be
mapped and painted. Each Boxes run asserts that all three observed windows
painted and that the clock belongs to the current window. Replacements retain
the requested width, including the final 720 px window.

The original first-paint startup timestamp remains the first window's timestamp.
Paint spans cannot cross window boundaries. These are passive frame observers;
there is no extra frame-driving callback or application polling. The original
20-second UI watchdog and fatal-warning policy remain unchanged.

`test-ui --trace` logs Boxes step transitions to stderr. Timeout diagnostics now
include the current frame counter, dialog visibility/mapping and import-button
geometry. The GNOME wrapper records elapsed seconds per case and supports
`--repeat 1–20`; the standard matrix at that stage had seven cases. Repeated cases and screenshots
have separate names/directories; custom commands cannot be repeated with this
option. The default remains one matrix.

## Evidence and limits

The [validation record](validation.json) retains the failed pre-fix regression,
source/binary hashes, repeated normal/sanitizer UI results, headless results,
trace excerpts and desktop cleanup. Meson process environments are omitted.

The historical step-4 timeout itself **was not reproduced** in this investigation.
Before the clock assertion was added, twelve repeated Boxes runs, both current
and historical seven-case matrices, four traced runs and two controlled
scan-before-map runs passed. The controlled case deliberately completed a scan
before showing the replacement window and also worked. That did not justify a
change to application presentation ordering.

The stale-clock assertion reproduces a separate, definite harness defect; it
does not prove the precise cause of the historical timeout. The broader
intermittent startup/dialog-readiness acceptance gate stays open. Repeated
successful runs alone are not a reliability sign-off. Future failures will have
current-window state and per-case elapsed time rather than closed-window frame
counts. No startup/performance improvement, physical input or screen-reader
acceptance is claimed.

## Reproduce

```sh
meson compile -C build
python3 scripts/test-gnome.py --build build --repeat 2 \
  --output build/qa/boxes-readiness-new
```

For a focused transition trace:

```sh
python3 scripts/test-gnome.py --output build/qa/boxes-trace-new -- \
  build/tests/test-ui --boxes-exercise --trace
```

Use `--build build-sanitize` and the matching runtime environment from the
[development instructions](../../implementation/setup.md) for ASan/UBSan.
Leak detection remains disabled for the previously documented third-party
initialization findings. All these cases use synthetic libvirt/Flatpak fixtures
in a private GNOME desktop; no real machine is imported or modified.

## Controlled discovery handoff follow-up

A separate control held the real discovery worker inside a FIFO-backed fake
`flatpak info`, opened Preferences, then released the worker. Its completion
left Preferences visible and offered the guide after Preferences closed. That
control passed before any application change and did not reproduce the
historical step-4 timeout.

Extending the control exposed a product bug: `update_actions()` disabled **New**
and native machine controls until Boxes discovery completed. The regression
failed at the assertion that New should be available while the scan was held.
Removing that read-only scan condition from native control/creation readiness
keeps those operations available once the native connection is ready. Import
still waits for discovery. No presentation-ordering, timer or deadline change
was needed.

The two new cases pause and resume a synthetic libvirt test-driver machine
while the scan is held. They then open Preferences (normal size) or creation
(200% text, dark, 720 px), release discovery, assert the current dialog remains
visible after the scan callback, close it and verify the guide appears. They
dismiss the guide and check persisted one-time state and the machine's running
state. The linker wrapper forwards the real finish call and counts callbacks;
it does not replace discovery results or scheduling. Runtime-local FIFO fixtures
and their child processes are cleaned up even after the pre-fix assertion abort.

The matrix now has nine cases. Two repetitions passed **18 GNOME cases per
normal and ASan/UBSan build**, including four controlled handoffs per build.
Both builds also passed **14 headless checks**. The
[handoff validation record](handoff-validation.json) includes the passing
pre-change modal control, failing availability regression, pinned source/binary
hashes, repeated results, logs and desktop cleanup. Layout captures:
[normal](handoff-normal.png), [200% text/dark](handoff-large-text-dark.png).

These checks use programmed GTK actions and synthetic guests. They do not
certify physical input, screen readers, real VM imports or startup performance.
The historical timeout's cause and broader readiness gate remain open.

For the focused controlled case:

```sh
python3 scripts/test-gnome.py --output build/qa/boxes-handoff-new -- \
  build/tests/test-ui --boxes-deferred-exercise
```

The [lifecycle/startup follow-up](../lifecycle-leaks/README.md) reproduces a
current ASan startup stall in a minimal GTK program and captures a stack in
NVIDIA's graphics library. Eighteen UI cases pass with an explicit Intel Vulkan
driver control. It does not establish the cause of the older Boxes step-4
timeout, and no application renderer override or watchdog extension was made.
