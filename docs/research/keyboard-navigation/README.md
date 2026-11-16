# Focus navigation validation

On 2026-11-15, frozen normal and ASan/UBSan builds each passed **13 headless
checks and seven GNOME UI cases**. The desktop was GNOME Shell 51.1 on Fedora
45, GTK 4.24.1 and libadwaita 1.10, with a 1280×800/60 Hz virtual monitor.
No sanitizer runtime errors were reported with leak detection disabled.

Pirwa returns focus to the machine list when search closes in a mapped, open
window without a modal dialog. The notification handler adds no polling or
runtime dependency. The regression was found while testing Escape after search.

The standard programmed GTK action matrix checks search-button and stop-search
closure, including focus restoration without an explicit test focus reset. The
creation dialog checks disabled Continue without an image, visible header/footer
labels, forward focus traversal to the ISO chooser, backward/forward traversal
inside the modal, and the focused chooser fitting its scroll viewport. The same
assertions run at normal and 200% text size in the narrow dark layout. No image
is selected and no VM is created by these focus checks.

The [validation record](validation.json) identifies the frozen source, binaries,
commands, headless checks, GNOME matrix results and process cleanup.
[Raw desktop logs](logs.tar.gz) retain the per-case output; Meson process
environments are omitted. The
[source archive](sources.tar.gz) retains the implementation under test. Layout
captures: [normal](normal.png), [large text/dark](large-text-dark.png).

## Scope and isolation

These are programmed GTK actions and focus traversal, not physical keyboard,
compositor shortcut delivery, AT-SPI/screen-reader or guest-input certification.
The matrix uses the libvirt test driver and a private GNOME Wayland desktop,
D-Bus, runtime/config/data/cache/state directories and memory GSettings. The
console cases use synthetic domains. Unexpected GTK/GObject warnings remain
fatal. No logging filter or renderer/protocol override ships with these tests.

The preference helper is included in the core library because both collection
and console use it. Concurrent creation/preferences work was frozen with the
rest of the tested source; hashes identify the exact implementation.

## Experimental input findings

Earlier private Mutter RemoteDesktop injection runs completed, but the same
sequence later stalled in an immutable build with no delivered key events.
They are exploratory evidence, not accepted input coverage. Vulkan/OpenGL
attempts also emitted GDK duplicate-presentation-time warnings. Software
rendering and protocol filtering did not establish repeatability. The temporary
injection framework and warning policy were removed from the shipping tests;
[experimental sources](experimental-input-sources.tar.gz) retain them for
investigation. No conclusion about physical input or presentation follows.

Physical input, screen readers and the broader real-guest console matrix remain
open. The prepared real-guest performance comparison also requires enough host
RAM; see the [renderer follow-up](../renderer-memory/README.md).

## Reproduce

```sh
meson compile -C build
python3 scripts/test-gnome.py --build build --output build/qa/focus-matrix
```

For the narrow layout alone:

```sh
python3 scripts/test-gnome.py --output build/qa/focus-one -- \
  build/tests/test-ui --exercise --large-text --dark --width 720
```

For address/undefined sanitizers, use `--build build-sanitize` with
`ASAN_OPTIONS=detect_leaks=0`. This host's extracted matching GCC runtime also
requires its temporary library directory in `LD_LIBRARY_PATH`. Existing
third-party initialization findings require leak detection to be disabled;
these checks do not establish leak freedom. Default Meson tests require neither
a desktop nor KVM.

References: [GTK focus traversal](https://docs.gtk.org/gtk4/method.Widget.child_focus.html),
[GTK accessibility](https://docs.gtk.org/gtk4/section-accessibility.html).
