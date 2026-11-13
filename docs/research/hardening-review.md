# Native UI and workflow hardening

Review date: 2026-11-13. Production changes are in commit `79d3312`; the
repeatable virtual GNOME wrapper is in `8cecaa7`. This remains a development
candidate with the release gates listed in v0.1-validation.md.

## Correctness findings and fixes

- Backend shutdown previously did not cancel a request submitted without a
  caller cancellable. Shutdown now feeds every worker request's cancellable.
  A FIFO-backed test-driver connection deterministically blocks, stops, and
  resumes the worker to verify cancellation without blocking GTK.
- Foreground results and lifecycle notifications could arrive through different
  main-loop sources out of order. Completion serials fence snapshots and
  connection errors. Detail generations prevent stale same-UUID replies; Settings
  refreshes details before opening. Progress is matched to its request ID.
- Rollback could delete storage after a failed start without proving that the
  domain was inactive. It now retains uncertain/running/shared resources and
  ownership evidence. Both rollback and normal removal rescan references after
  undefining to detect a racing transient domain. Scans handle inode aliases,
  backing references, missing unrelated media, and ambiguous/inaccessible paths.
  Cross-client libvirt transactions still cannot be made atomic.
- The creation header clipped its primary action at large text sizes. Shorter
  Create text, a wider dialog, wrapping resource/error labels, and explicit
  accessible header names keep controls usable at 200% text and 720 px width.
- The loading spinner now stops when its page is hidden. Giving initial focus
  to the VM collection avoids animating an unused search cursor; Ctrl+F focuses
  search when requested. Short diagnostic samples then recorded no idle frames.
  This alone does not establish the full 60-second performance budget.

## Virtual GNOME UI test

From the project root, after building:

    ./scripts/test-gnome.py --output build/qa/virtual-gnome

The wrapper runs GNOME Shell headless with a 1280×800, 60 Hz monitor and a private
Wayland socket/D-Bus session. Runtime/config/data/cache directories and GSettings
are disposable. It runs the light, 200% font/dark/narrow, and empty-inventory
cases, saves app-only screenshots and per-case reports, stops the compositor,
and cleans up test-session FUSE mounts. The host desktop settings are unchanged.

The matrix also passed with the sanitizer binary using the extracted runtime
environment documented in setup.md. All eight default checks passed normally
and with address/undefined sanitizers; leak detection remains disabled. The
isolated real-KVM integration passed after the storage/cancellation changes,
and the existing session guests remained stopped.

The virtual session is a compositor desktop, not a full hardware login. Its logs
record unavailable AT-SPI activation and optional authentication/camera/PipeWire
services. The UI exercise uses GTK's test accessibility backend. It verifies
programmed actions and explicit accessible properties, with no claim about
screen-reader behavior, physical input, viewer audio, or guest installation.

Saved evidence: [normal report](virtual-gnome/report.json),
[sanitizer report](virtual-gnome/sanitized-report.json),
[normal compositor log](virtual-gnome/gnome-shell.log),
[sanitizer compositor log](virtual-gnome/sanitized-gnome-shell.log),
[light screenshot](virtual-gnome/light.png),
[200% dark screenshot](virtual-gnome/large-dark.png), and
[empty-inventory creation screenshot](virtual-gnome/empty.png).
The reports mark the wrapper's then-uncommitted state; production C code is
the committed `79d3312` tree. The only later C change adds timeout diagnostics
to the QA tool.

## Performance interpretation

The physical Wayland run used the optimized production window/backend in the
non-unique QA application, 100 synthetic test-driver domains, one warmup and
30 measured launches. Launch-to-after-paint measured p50 430.043 ms and p95
480.118 ms. After-paint is submission, not compositor presentation, and the QA
application excludes production single-instance activation.

Its 60-second idle sample measured 0.639% of one core, 395 frame-clock cycles,
and 144,958 KiB PSS (141.6 MiB) with Vulkan on a 240 Hz monitor. Window activity
changed during the sample. These results do not pass the provisional idle CPU
or memory budgets; the test-driver domains also live inside the process rather
than a real libvirt daemon. Keep this physical run separate from controlled
virtual-desktop measurements.
The [raw physical samples](native-ui-physical-100.json) retain environment,
binary hash, diagnostics, and every launch value.

The controlled virtual GNOME repeat used the same optimized UI with a 60 Hz
virtual monitor and GTK's test accessibility backend. Its 30 launches measured
p50 432.717 ms and p95 502.208 ms to after-paint. A 60.015-second idle sample
measured 0.002% of one core, zero frame-clock cycles, and 102,027 KiB PSS
(99.6 MiB). The window was inactive with focus retained on a list item. These
are an inactive-window synthetic baseline, not a physical desktop or production
accessibility acceptance result. The p95 also slightly exceeds the provisional
500 ms startup target. See [raw samples](virtual-gnome/native-ui-100.json) and
[profile session report](virtual-gnome/profile-report.json). The dirty flag in
this repeat reflects the QA timeout diagnostic and review documentation; the
production window/backend did not change.

An earlier run in virtual GNOME timed out waiting for UI readiness on the 12th
measured launch. The full repeat passed after adding timeout diagnostics, with
no production fix. Treat this as an unresolved intermittent startup finding,
not a resolved defect or a passed reliability gate. The watchdog now prints
mapping, readiness, frame, renderer, size, and activity state on future failures.
Saved evidence: [failure report](virtual-gnome/startup-failure-report.json),
[watchdog trace](virtual-gnome/startup-failure.log), and
[partial raw samples](virtual-gnome/startup-failure-partial.json).

Follow-up at `c60ee96`: [100 measured launches](boxes-import/README.md#startup-follow-up)
completed without a timeout. The original intermittent cause remains unresolved.

Short renderer diagnostics showed lower launch cost/PSS with Cairo and lower
scroll CPU work with Vulkan. They are investigation samples, not sufficient
evidence for a universal renderer override. Pirwa retains GTK's default selection.
Cold caches, physical presentation latency, real-daemon memory, guest throughput,
and full viewer/install acceptance remain open.
