# Machine avatar validation

Validated on 2026-11-16 with GTK 4.24.1, libadwaita 1.10.0 and an isolated
virtual GNOME Wayland desktop. All guest operations use `test:///default` or
synthetic test-driver inventories. No real guest, ISO, daemon or KVM is used.

The current feature includes 16 original robot expressions, a shared theme-aware
monitor frame, a searchable gallery created on first expansion, selection during creation,
and editing through Settings. Icons are local preferences keyed by connection
and UUID. The preset source and licensing are in
[the asset README](../../../data/avatars/README.md).

## Correctness and presentation

- Native Meson build and all 23 default headless checks passed.
- `test-avatar-ui` passed in light mode and in dark mode at 720 pixels with
  doubled text. It checks search, Cancel, saving while running, unknown-preset
  fallback, reopening a window, and restoring the generic icon. Its backend
  wrapper rejects guest-edit requests from these cosmetic saves.
- `test-create-ui` passed in light mode and in dark mode at 720 pixels with
  doubled text. The selected icon survives wizard navigation and appears in
  the final summary and create-request callback.
- `test-ui --exercise` passed the combined name/CPU/avatar edit workflow.
- `test-ui-lifetime --cycles 8` passed with all observed windows, preferences,
  models, dialogs, consoles and backends finalized.
- All SVGs parse and contain no scripts, raster images, text elements or external
  image references. Staged installation includes the embedded icons, preference
  schema and asset licensing documentation. Formatting and whitespace checks
  passed.

Reproduction commands for avatar checks are in
[setup](../../implementation/setup.md). Screenshots and full desktop logs are
under `build/qa/avatars-final-light`, `build/qa/avatars-final-dark`,
`build/qa/avatars-create-verified` and `build/qa/avatars-create-dark`.

## Robot expression revision

The distro-logo prototype was replaced on 2026-11-16 with 16 original minimal
robot faces. Color belongs to the face's screen; the outer monitor follows the
theme. Each face is a static SVG with simple eyes and a small mouth. User
choices remain stable when VM state changes. The earlier font-logos assets and
their packaging license entries have been removed.

The native build, 23 headless checks, `test-avatar-ui` in light/dark mode
(including 720-pixel width and doubled text), and `test-create-ui` passed for
this revision. The tests select Cheerful and verify that old distro identifiers
fall back to Friendly. The selected face is shown in the creation summary. The combined name/CPU/avatar
edit workflow also passed with the new expressions.
Screenshots are under `build/qa/robot-faces-light`, `build/qa/robot-faces-dark`
and `build/qa/robot-faces-create`. A fresh staged installation includes only
the original Pirwa artwork documentation, without the old font-logos license.

### Transparent features and running color

The later design correction removes the colored head/background shape while
keeping the eyes and mouth directly inside the monitor frame. All artwork is
symbolic; curved feature strokes are expanded into filled outlines for GTK's
symbolic renderer. Both frame and facial features use libadwaita's standalone
accent foreground while running, and the normal foreground while paused or
stopped. The expression itself remains the user's choice.

The native build and 23 default tests passed. The light/dark avatar checks
also exercise running → paused → resumed → stopped transitions, assert the
accent class is set and cleared, and check that computed foreground colors
change. Final screenshots are under `build/qa/monitor-faces-final-light` and
`build/qa/monitor-faces-final-dark`. No state timers or new dependencies were
added.

An early popover prototype triggered a GDK presentation-feedback warning on
this GTK build. The final gallery expands inline in Settings and in the Name
step. Its successful checks use the normal Wayland backend and renderer,
with warnings remaining fatal and without disabling presentation feedback.

## Running screen fill revision

The latest design fills the entire monitor screen with the preset's color
only while running. A shared SVG mask sits under the bezel, avoiding the inset
head shape. Paused/stopped machines and picker previews have clear screens.
The earlier whole-icon accent tint is removed: the frame retains its normal
foreground color, and running faces use dark features over the light fill.
The desktop check asserts fill opacity across pause/resume/stop and verifies
that the frame's computed foreground stays unchanged. This adds one static
image layer per avatar and no timers or runtime dependencies.
The native build, 23 headless checks, formatting, and light/dark desktop checks
passed. Screenshots showing running and paused states are under
`build/qa/running-screen-light/screenshots` and
`build/qa/running-screen-dark/screenshots`. The 1,000-machine scrolling smoke
check also completed; samples and environment are in
[running-screen-scroll.json](running-screen-scroll.json). It establishes no
performance improvement or physical-display acceptance result.

## Collection smoke check

The earlier distro-logo prototype's profiler completed three warm launches
with 1,000 synthetic machines and five seconds of scrolling. Numerical samples, binary/schema
hashes, source hashes and environment are retained in [ui-scroll.json](ui-scroll.json).
Repeated missing-OS-metadata diagnostics from the synthetic fixtures are
omitted from that copy.

```sh
./scripts/test-gnome.py --output build/qa/avatars-final-scroll -- \
  env GTK_A11Y=test ./scripts/profile-ui.py --build build --domains 1000 \
  --launches 3 --idle-seconds 0 --scroll-seconds 5 \
  --output build/qa/avatars-final-scroll/profile.json
```

This is a functional scrolling smoke check. Other isolated desktop checks ran
concurrently, there is no matched baseline, and three launch samples do not
establish a launch percentile. These timings do not establish a performance
improvement or satisfy the physical-display performance acceptance gates.

The robot-face revision also completed the same 1,000-machine smoke check in
an isolated virtual GNOME session, with `--display-kind virtual` added to the
profiler invocation. Its samples and environment are retained in
[robot-faces-scroll.json](robot-faces-scroll.json). This run verifies the
collection workflow; three warm samples and no matched baseline establish no
performance improvement or physical-display acceptance result.
