# GTK 4 SPICE console validation — 2026-11-14

Development evidence on Fedora 45, GNOME Shell/Mutter 51.1, GTK 4.24.1,
libadwaita 1.10, libvirt 12.6 and SPICE GLib 0.43. The guest is Fedora Workstation
Live 43, with four vCPUs, 4096 MiB RAM, a disposable 16 GiB volume and software
SPICE/virtio graphics. This does not replace the wider release acceptance matrix.

## Checks

- All ten default Meson checks pass, including new immutable frame/format/input
  mapping checks, native-vs-external console selection, unsupported graphics FD
  errors, and reply-owned FD cleanup. They need no display, daemon or real VM.
- All ten pass with AddressSanitizer and UndefinedBehaviorSanitizer
  (`ASAN_OPTIONS=detect_leaks=0`, following the existing libvirt leak limitation).
- Five existing manager UI scenarios pass in an isolated GNOME Wayland desktop.
- Open Console displays the actual Fedora boot and GNOME desktop in a GTK 4
  libadwaita window. Process maps contain GTK 4 and SPICE GLib, with no GTK 3.
- Pointer clicks open the guest terminal. Injected keyboard input prints
  `PIRWA_NATIVE_INPUT_OK`; Ctrl+Alt releases input and recapture prints
  `PIRWA_NATIVE_RECAPTURE_OK`. GNOME's shortcut inhibition prompt was accepted.
- Fullscreen changes the actual guest framebuffer from 1024×692 to 1280×724,
  measured using libvirt screenshots, and returning to a window restores 1024×692.
- A six-second 440 Hz WAV generated in the guest plays through SPICE to the
  private host null sink. Digital capture contains 157084 nonzero left-channel
  samples; a one-second analysis window peaks at 440 Hz in the 430–450 Hz range.
  This checks transport, not physical speakers or latency.
- Stopping the uniquely named QA VM shows “Machine is stopped.” Starting it
  reconnects the same native window and reaches Fedora's desktop again.
- Closing the collection leaves the native console running; activating Pirwa
  again opens a new collection window without stopping the console.
- Six close/reopen cycles each return to 60 process FDs with the console closed
  and 83 with it open. No upward FD trend was observed.
- A 30-second idle sample with the native console and collection open measured
  0.067% of one core and 162.8 MiB PSS. The guest display was inactive in the
  following screenshot, so this is not an active-frame performance baseline.

Raw screenshots, audio, FD counts, baseline hashes and measurement JSON are in
`build/qa/native-console/` (ignored build artifacts). `control/final-native.png`
shows the final executable. The disposable VM, disk and pool were removed;
both existing Boxes XML hashes and power states match the pre-test baseline. Earlier installation and external-viewer results
remain in their own records; they do not validate every native-console feature.

## Reproduction

Use the existing isolated desktop controller, with the binary set to the current
build. Warm the host session libvirt socket first. Create only a uniquely named
`pirwa-qa-<UUID>` directory pool and guest; never use an existing user's VM for
power or input tests. The controller's `fixture` action invokes `tests/qa-vm`,
which independently enforces the disposable identity and explicit opt-in.

```sh
virsh -c qemu:///session list --all
PIRWA_RUN_KVM_TESTS=1 ./scripts/test-gnome.py --command-timeout 3600 \
  --output build/qa/native-console/desktop -- \
  python3 scripts/console-session.py --binary build/src/pirwa \
    --output build/qa/native-console/control \
    --uri 'qemu+unix:///session?socket=/run/user/1000/libvirt/virtqemud-sock'
```

From another terminal, send JSON requests with
`python3 scripts/console-session.py --output build/qa/native-console/control --request JSON`.
Use `launch`, native row selection and `app-action` `console`, then the input,
screenshot and identity-checked fixture actions. The socket URI above is specific
to this host's UID and daemon. The wrapper creates a private bus, display, settings,
runtime and audio sink; its cleanup targets only that private runtime. Stop/remove
the disposable VM, destroy its empty pool, and send `exit` before finishing.

## Remaining gates

Accelerated SPICE GL configurations retain virt-viewer. Native DMABUF/GL,
multiple monitors, clipboard, locked relative pointer input, microphone, physical
input/audio, presentation frame times, broader active-application CPU profiles, and a broader
host matrix remain open. This validation revision used full-surface copies for
each rendered update. The later [renderer profiling and sparse-update work](../display-performance/README.md)
records the bounded tile cache and synthetic before/after results. It does not
establish a performance improvement over virt-viewer or real-guest performance.

The later [active guest profile](../active-console-performance/README.md) adds
controlled 1080p and 4K GNOME drawing, real SPICE invalidation/copy measurements,
and an explicit dense-frame tail-cost follow-up.
