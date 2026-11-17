# Pirwa agent instructions

Pirwa is a native GNOME virtual machine manager. Performance and responsiveness
are product requirements. Use C17, GTK 4, libadwaita, Meson, and the libvirt C API with libvirt-glib event integration, libxml2, and libosinfo.
SPICE consoles use spice-client-glib without linking GTK 3.
The project setup, v0.1 plan, and its implementation are authorized by the user.

## Layout

- `src/`: application, GTK UI, immutable VM snapshots, and libvirt backend.
- `tests/`: deterministic GLib tests using libvirt's `test:///default` driver.
- `data/`: desktop/AppStream integration and application icon.
- `packaging/`: native Fedora RPM recipe.
- `po/`: gettext catalogs and translation setup.
- `.github/workflows/`: automated native and sanitizer checks.
- `docs/design/`: architecture and first-version scope.
- `docs/implementation/`: setup and technical workflow.
- `docs/research/`: source-backed investigations and reference review.
- `docs/user/`: user-facing instructions.
- `scripts/`: repeatable development tools.
- `examples/`: runnable examples.
- `postmortem/`: incident reports when needed.

## Workflow

Read the relevant design and implementation docs before changes. Build a small
end-to-end slice, verify it, and update docs with behavior changes. Stay within
the requested scope. Explain new dependencies and architectural changes.

Use these commands from the repository root:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
clang-format --dry-run --Werror src/*.[ch] tests/*.c
```

Use `meson setup build --reconfigure` for an existing build directory. Tests must
not require KVM, root, a display server, a running daemon, or real guest changes.
Use the opt-in test-kvm executable only with PIRWA_RUN_KVM_TESTS=1. It creates uniquely named disposable resources. Run applicable checks before finishing. Use `git` for version control.

## Code and performance

- Keep all GTK work on the main thread. Blocking libvirt and filesystem calls
  belong on workers. Never shell out to `virsh` to implement application logic.
- Keep libvirt handles in the backend. Transfer owned snapshots to the UI; update canonical model objects only on the main thread.
- Prefer bulk queries and virtualized GTK list models. Do not add per-VM polling
  or screenshot timers to the collection screen.
- Bound concurrent work and make pending work cancellable. Synchronous libvirt
  calls cannot be interrupted by `GCancellable`; discard cancelled results.
- Preserve each libvirt failure before cleanup calls can replace its error.
- Close handles and free allocations on every exit path. Follow existing style.
- Do not claim a performance improvement without a repeatable measurement.
- Never change existing VM definitions merely by discovering them. Disk deletion
  requires proven ownership and an explicit user operation.
- Keep the app native; there is no JavaScript toolchain in this project.

## UI rules

- Keep the desktop font and native libadwaita title sizes. Creation-page headings
  use `title-1` with `pirwa-create-title` to set bold weight (700); the default
  extra-bold weight (800) crowds Latin letters with Noto Sans CJK HK.
- Preserve natural letter spacing. Fix the font weight before adding tracking or
  replacing the user's font to compensate for crowded headings.
- Check heading readability with the default font and Noto Sans CJK HK, including
  dark appearance and 200% text size. Titles must wrap without clipping.

## Documentation

Architecture belongs in `docs/design/`, implementation in `docs/implementation/`,
and user workflows in `docs/user/`. Keep README commands runnable. Add examples
when introducing a workflow. Incident reports describe cause, fix, and prevention.
Use plain language and keep functions small and composable.

## CodeGraph

Prefer CodeGraph for structural questions when an index exists. Use
`codegraph_context` then one `codegraph_explore`, where available. Do not repeat
AST results with grep. Use native search for literal text and known files. If
`.codegraph/` is absent, ask before running `codegraph init -i`; do not create an
index without authorization. Ignore the local index in Git.
