# Console and performance checks — 2026-11-13

This follow-up uses the existing production application code at `415f466` plus
new QA tools. It covers one Fedora host/guest combination in a private GNOME
Wayland desktop. Raw reports retain the source revision, dirty-tree marker,
binary/workload hashes, renderer, packages, power profile, and individual samples.
The application continues to use GTK's default renderer.

## Console

The guest uses Fedora Workstation Live 43-1.6, KVM, host-passthrough CPU, four
vCPUs, 4096 MiB RAM, UEFI, virtio devices, and local SPICE. The disposable disk
is 16 GiB. The existing Boxes VMs are excluded from all mutations.

- **Resize:** with the normal viewer's default auto-resize enabled, a libvirt
  screenshot changed from 1280×721 to 1144×656 after window resizing. These are
  guest framebuffer dimensions, rather than scaled desktop screenshot sizes.
  See the [before](guest-frame-before.png) and [after](guest-frame-after.png) frames.
- **Input release:** Ctrl+Alt released the viewer grab and allowed host window
  controls. Moving outside the display and clicking back restored guest input;
  the guest's marker file changed from `before` to `beforeafter` in the same
  viewer session. See [input.json](input.json). These are injected desktop events;
  physical keyboard/device capture remains a separate acceptance check.
- **Audio transport:** the guest generated two seconds of 440 Hz, 48 kHz stereo
  PCM. Its own `pw-play` sent the tone through SPICE and the normal viewer into
  a private PipeWire null sink. The sink monitor capture contains a strong 440 Hz
  peak and an RMS of about 3657 in the loudest 100 ms interval. See [audio.json](audio.json).
  This verifies digital transport, not speakers, microphones, or latency.
- **Reconnect:** the same viewer PID remained alive through a graceful guest
  shutdown and Start, displaying the new boot without a client relaunch.
  See [reconnect.json](reconnect.json).

The private audio core uses an explicit minimal configuration and WirePlumber's
`policy` profile, with no hardware monitoring or network sink discovery. Its
only sink is the QA null sink. Default Meson checks do not start these services.

## Collection and start measurements

Release build, Fedora 45, Intel Core Ultra 9 185H, GNOME Shell 51.1, virtual
1280×800/60 Hz monitor, default `GskVulkanRenderer`. These are synthetic libvirt
inventories in the shared production window/backend, with GTK's test
accessibility backend. They exclude production single-instance bus activation,
physical presentation, and cold caches.

| Measurement | Result | Provisional budget | Scope |
| --- | ---: | ---: | --- |
| Warm startup, 100 domains | p95 496.378 ms | ≤500 ms | 30 launches after one warmup, through after-paint |
| Warm startup, 1,000 domains | p95 448.507 ms | ≤500 ms | Separate 30-launch run |
| Idle CPU, 100 domains | 0.0019% of one core | <0.5% | All-thread CPU over 60.03 s |
| Idle PSS, 100 domains | 96.1 MiB | ≤100 MiB | Manager process, excludes daemon/viewer/QEMU |
| Scroll frame CPU work, 1,000 domains | p95 3.701 ms | 16.7 ms presentation target | 10 s scroll; submission work is a proxy, not presentation acceptance |
| Start backend overhead | p95 27.127 ms | ≤250 ms | 30 alternating paired starts of the same domain |

The startup and memory results are close to their limits. They do not justify
a universal renderer override or establish that Pirwa is faster than Boxes.
See [100-domain samples](profile-100.json), [1,000-domain samples](profile-1000.json),
[start samples](start-samples.jsonl), and [start summary](start-summary.json).

Start timing compares a persistent direct libvirt `virDomainCreate` with
completion of a persistent production-backend `PIRWA_START` request, including
its inventory refresh. Both use the same definition and alternate first/second
order. The test stops the uniquely identified disposable guest after each
sample; guest boot and display connection are outside the measured interval.
This is backend overhead, not physical button-to-display latency.

## Guest workload method

The workload comparison keeps the same VM running while alternating the manager
closed and idle, isolating manager presence from boot/cache variation. Start
method overhead is measured separately above. This does not claim to compare
cold installed-guest boots or a console-attached workload.

A dedicated test-only virtio channel runs `tests/fixtures/qa-channel-agent.py`
inside the disposable guest. The host verifies that its blank `/dev/vda` maps
to the owned test volume before formatting that virtual disk as ext4. The guest
also checks its exact capacity, disk type, children, and mount state before
formatting. Neither the agent nor this disk preparation is a product feature.

`scripts/profile-guest.py` runs a warmup and 12 paired samples in alternating
order. Each sample preconditions a fixed-seed 1 GiB file, measures a sequential
1 GiB write plus fsync and repeated reads with guest `O_DIRECT`, then measures
fixed-input PBKDF2-HMAC-SHA256 iterations. Every CPU digest must match.
The same settling period is used in both manager conditions.
Host caches stay warm; no host-wide cache drop or disk-cache setting is changed.
The storage measurements therefore do not certify raw device throughput.

The [initial run](guest-workloads.json) used one read pass, 6,000,000 CPU
iterations, and a two-second settling period. Median paired changes with the
manager idle were −1.45% CPU duration, +0.13% write throughput, and −5.97% read
throughput. The read result crossed the provisional 5% investigation threshold.
Its individual paired read ratios ranged from 0.558 to 1.092, and reads lasted
about 0.6 seconds. The [original workload](initial-workload.py) is retained.

A separate [longer run](guest-workloads-long.json) uses eight read passes,
24,000,000 CPU iterations, and a five-second settling period to reduce timing
noise. Its parameters and workload hash are recorded separately. These cohorts
must not be combined as equivalent workloads or used to claim an application
speedup. The current tool exposes all three parameters explicitly.

The longer run's median paired changes were +0.71% CPU duration, −8.55% write
throughput, and +0.95% read throughput. Paired write ratios ranged from 0.648 to
1.561; read ratios ranged from 0.786 to 1.177. The initial read finding did not
persist, and storage varies substantially. Storage acceptance remains open;
these samples do not establish either an application regression or a speedup.

The [A/A control](guest-baseline-control.json) repeats three adjacent pairs with
the manager closed throughout. Paired write changes range from −33.4% to +14.1%,
and read changes from −11.7% to +5.5%, exceeding the investigation thresholds
without Pirwa running. A quieter, controlled storage reference is needed before
accepting or attributing the storage results. No cache or renderer change was
made to make these results pass.

## Repeatable tooling

The default nine checks remain synthetic. Real-guest helpers require explicit
`PIRWA_RUN_KVM_TESTS=1`, and `qa-vm` refuses domains whose names are not
`pirwa-qa-` followed by a UUID. Use a unique directory pool and record baseline
definitions/states before preparing a guest.

```sh
meson compile -C build-perf
PIRWA_RUN_KVM_TESTS=1 ./scripts/test-gnome.py --command-timeout 7200 \
  --output build/qa/acceptance/desktop -- \
  python3 scripts/console-session.py --output build/qa/acceptance/control \
  --uri '<explicit host session socket URI>'
```

Install virt-viewer normally, or use the controller's `--viewer-dir` for a
verified temporary dependency prefix. The controller provides a private audio
core, screen consumer, keyboard/pointer events, and screenshots. It never creates
a guest automatically. Requests can be sent from another terminal:

```sh
python3 scripts/console-session.py --output build/qa/acceptance/control \
  --request '{"action":"snapshot","name":"desktop.png"}'
```

`fixture` requests invoke the compiled `qa-vm` helper with the controller's
private ownership records. That helper supports create/import/start/stop/remove,
console, and `benchmark-start UUID PAIRS`. Creation requires an ISO or standalone
qcow2 plus the unique pool name. Remove uses Pirwa's verified ownership checks.
These are opt-in test mutations; they must never target an existing user VM.

For guest workloads, attach a private Unix/virtio channel named
`org.pirwa.qa.0`, then run the agent as root inside the disposable guest with
`PIRWA_RUN_KVM_TESTS=1`. It accepts JSON argv arrays, has no network listener,
and must only be used for a disposable guest. Prepare the owned disk and create
`/mnt/pirwa-qa/.pirwa-qa-disposable` before sampling:

```sh
PIRWA_RUN_KVM_TESTS=1 python3 scripts/profile-guest.py \
  --channel '<QA channel socket>' --controller build/qa/acceptance/control \
  --pairs 12 --output build/qa/acceptance/guest-workloads.json
```

Finish by stopping/removing the test guest and its verified disk, verifying the
pool is empty before removing it, comparing the baseline, and sending the
controller `{"action":"exit"}`. VM cleanup is the caller's responsibility.

Two fixture issues were corrected during this run: inherited viewer output
pipes could block the controller, and desktop process groups were isolated
from the outer launcher after an initial SIGTERM teardown failure. A terminal's
container helper also survived its bus. The wrapper now reaps only same-user
processes carrying the exact private runtime, using PID handles, before removing
its directories. An intentionally orphaned helper verifies that behavior in the
[cleanup check](process-cleanup.json). The corrected controller and UI matrix
exit normally. These changes do not alter normal application behavior.

Final cleanup's ACPI shutdown did not finish within 30 seconds after the extra
login-session checks. The test filesystem had already been synced and unmounted;
the disposable guest was explicitly force-stopped, then removed through Pirwa's
verified ownership path. The guest, volume, and empty pool are absent, and both
existing Boxes definitions/states match their baseline. See [cleanup.json](cleanup.json)
and the [acceptance summary](report.json).

## Remaining acceptance

Cold startup, physical presentation/scrolling traces, real-daemon large
inventories, slow/disconnected-backend main-thread traces, microphone and
clipboard forwarding, physical input/accessibility, and a broader guest/host
matrix remain open. The known libvirt initialization leak finding remains;
address/undefined sanitizer checks use the documented `detect_leaks=0` setting.
The measurements here do not close every release gate.
