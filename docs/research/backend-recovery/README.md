# Backend recovery validation

On 2026-11-15, normal and ASan/UBSan builds each passed **14 default checks**,
including all seven backend fault cases. The normal GNOME matrix passed seven
UI cases. The first sanitizer matrix timed out at Boxes step 4 while the reopened
import dialog was waiting to map; the identical pinned executables passed all
seven cases in a fresh desktop on the repeat. No sanitizer runtime error was
reported. The dialog/startup-readiness finding remains open.
The later [readiness investigation](../boxes-readiness/README.md) reproduces and
fixes stale frame-clock tracking in the UI harness. It retains the original
timeout as unconfirmed rather than treating passing repeats as proof of a fix.

The deterministic fault tests reproduced and fixed three backend recovery bugs:

- A canceled queued request advanced the reconnect counter even though it made
  no connection attempt. Cancellation now preserves the remaining retry budget.
- Reconnect after an exhausted budget attempted once, then stopped immediately
  if the service was still unavailable. An explicit reconnect now starts a fresh
  bounded retry cycle.
- Automatic recovery returned machine inventory without host/storage-pool data.
  A newly opened connection now supplies that data so creation can use the
  replacement connection's pools.

The changes stay on the existing worker. Production retry delays remain
1/2/4/8/16 seconds. There is no new connected-state polling, dependency or public
API. Failed power operations are never replayed during recovery.

## Test coverage

`test-backend-faults` uses the real libvirt test driver for connections, domain
handles, bulk inventory, metadata and lifecycle registration. Linker wrappers
inject only faults, connection-close registration/notifications and controlled
queue waits. Each case runs in its own subprocess. Successful connections are
closed, callback data is released, and the worker is joined by the test. The
application still shuts down without joining on the main thread.

| Case | Assertion |
| --- | --- |
| Startup failure/recovery | Two failed opens recover automatically; the snapshot has host/pool data and a newer completion serial |
| Exhausted retry budget | Exactly five increasing delays, then an indefinite queue wait; manual reconnect rearms all five attempts |
| Canceled queued work | No extra open, no consumed attempt; Stop wakes an outstanding retry wait |
| Close notification | Reconnect is queued off the callback, the old handle is closed, and replacement metadata is returned |
| Permission failure | Original denial survives cleanup overwriting libvirt's last error; the healthy connection is reused without retrying the command |
| Lost transport during Start | Connection recovery succeeds while the Start call count remains one |
| Blocked bulk inventory | The main loop dispatches an idle callback; Stop returns before the blocked call is released; the late completion is canceled |

Tests inspect the requested production delays, then shorten them to one
millisecond inside this executable. Held waits use the real queue so commands
and Stop can wake them. Progress watchdogs detect deadlocks; they are not
performance thresholds. No test opens a real VM, restarts a daemon, changes
permissions, launches a viewer or requires a display/KVM.

The [validation record](validation.json) retains the three pre-fix failures,
normal/sanitizer suite results, pinned UI executable hashes, GNOME matrix
reports, source hashes and desktop cleanup. Meson process environments are
omitted. The base commit is `f332a02`; source was uncommitted. Other existing
creation, console, preferences and icon changes were preserved.

## Reproduce

```sh
meson setup build --reconfigure
meson compile -C build
meson test -C build backend-faults --print-errorlogs
meson test -C build --print-errorlogs
python3 scripts/test-gnome.py --build build --output build/qa/recovery-ui
```

For the address/undefined build, use the sanitizer setup in
[development instructions](../../implementation/setup.md). This host also needs
its extracted matching GCC runtime in `LD_LIBRARY_PATH`. Leak detection remains
disabled for the previously recorded third-party initialization findings; these
checks do not establish leak freedom.

## Remaining acceptance

The private-daemon follow-ups below cover empty-session process restart,
Unix socket permission denial and private socket activation with the default
Session URI. The Linux guest follow-up also covers synced ext4 writes and
running-guest continuity across daemon replacement. System polkit, installed
systemd unit behavior and broader guest/storage combinations remain unvalidated. Main-loop dispatch during a
blocked test call is not a Sysprof trace or a GTK frame-time measurement. Those
release gates remain open, as do the existing physical input/accessibility and
real-guest performance gates. The separate intermittent GNOME startup-readiness
finding has not been closed by these tests.

## Private QEMU daemon follow-up

On 2026-11-15, the production backend passed five checks against a real
unprivileged **virtqemud 12.6.0**, both normally and with ASan/UBSan:

1. Start while the private daemon is absent, then recover automatically after
   it appears, receiving a newer inventory with host/pool fields.
2. Stop the private daemon gracefully and recover automatically after restart.
3. Kill that daemon and recover automatically after replacing it.
4. Deny access to its private socket, observe the actual permission error, then
   recover automatically when access is restored.
5. Dispatch a main-loop ping while the daemon is unavailable, stop during real
   retry backoff, and reject subsequent requests as canceled.

There are no libvirt RPC, event or timer wrappers in this executable. The only
linker wrapper retains the backend thread for a test-only join. Production
retry delays are used. This is backend transport evidence without GTK, a guest
or a performance claim. A separate SIGTERM run verified worker/daemon cleanup
and removal of the private directory. Both entry points refused to run without
the explicit opt-in. The normal and sanitizer default suites each still passed
all fourteen checks; this daemon test is not registered in the default suite.

The [live validation record](daemon-validation.json) includes all events,
daemon generations, source/binary hashes, client logs, guard checks and cleanup.
Each successful run ended with no private processes or VM/storage definitions
and its temporary directory removed. Existing creation, tray/media and console
work was preserved; per-run hashes identify the uncommitted source under test.

The supervisor supplies private XDG directories and a restricted environment,
uses an explicit `qemu+unix` socket, and sets `LIBVIRT_AUTOSTART=0`. It starts
only its own daemon in the foreground. Authentication is disabled only on that
private, owner-only session socket. No host service, VM definition, disk, pool
or network is changed. Session socket permission denial is not polkit coverage.

```sh
meson setup build --reconfigure
meson compile -C build test-backend-daemon
PIRWA_RUN_DAEMON_TESTS=1 ./scripts/test-libvirt-daemon.py \
  --build build --output build/qa/private-daemon-new
```

Choose a fresh output directory. The supervisor requires Python 3 on Linux and
installed `virtqemud`/QEMU-driver packages, and refuses root because the socket
permission check requires an unprivileged client. It needs no running libvirt
service, display or KVM. No application dependency or runtime behavior changed.

References: [session daemon paths](https://libvirt.org/daemons.html#modular-sockets),
[explicit socket URIs](https://libvirt.org/uri.html),
[disabling client autostart](https://libvirt.org/html/libvirt-libvirt-host.html#virConnectOpen).

References: [libvirt close callback lifecycle](https://libvirt.org/html/libvirt-libvirt-host.html#virConnectRegisterCloseCallback),
[libvirt error handling](https://libvirt.org/errors.html).

## Private socket activation follow-up

On 2026-11-15, the new `--socket-activation` mode passed five checks in both
normal and ASan/UBSan builds. The original direct-socket mode also passed all
five checks per build, and both default headless suites still passed fourteen
checks. No production code, retry delay or application dependency changed.

The supervisor starts `systemd-socket-activate` with an owner-only private
listener and the `virtqemud.socket` descriptor name. Before creating a client,
it checks that this process is still waiting for a connection. The production
backend connects using **`qemu:///session`**, with no explicit socket URI. A
successful empty inventory with host/storage metadata must coincide with that
same process becoming `virtqemud` and retaining the listening socket's inode.
The checks repeat that activation after graceful and forced daemon exit,
verify automatic recovery from actual socket permission denial, and dispatch a
main-loop ping before stopping during real retry backoff.

Only the fixture's restricted environment is forwarded to the activated daemon.
The test client requires its private runtime, config, data, cache and state
paths before accepting the default-URI mode. `LIBVIRT_AUTOSTART=0` prevents
libvirt's separate client-spawn mechanism. All daemon generations and helper
processes are tracked and cleaned up. A separate control stops an unused
listener before any client connects; another sends SIGTERM to the supervisor
after activation. Both controls left no private processes or sockets, removed
the temporary root, and the interrupted client joined its backend worker.

The [activation validation record](socket-activation-validation.json) retains
the four successful runs, process/executable transition assertions, source and
binary hashes, client/daemon logs, opt-in and isolation guards, cleanup controls,
and allowlisted headless results. The interrupted run is recorded as an expected
failure, rather than a completed test matrix. Leak detection remains disabled
for the previously documented third-party initialization findings.

```sh
meson compile -C build test-backend-daemon
PIRWA_RUN_DAEMON_TESTS=1 ./scripts/test-libvirt-daemon.py \
  --socket-activation --build build --output build/qa/socket-activation-new
```

This validates the LISTEN_FDS handoff and ordinary Session URI inside an empty
private environment. It does not validate installed systemd units, boot/login
enablement, system polkit or running-guest durability. No existing host service
or guest is operated, and no VM, disk, storage pool or network is defined. The
GNOME readiness, physical-input and performance gates remain separate.

References: [systemd activation test tool](https://www.freedesktop.org/software/systemd/man/latest/systemd-socket-activate.html),
[virtqemud startup modes](https://libvirt.org/manpages/virtqemud.html#daemon-startup-modes),
[Session sockets](https://libvirt.org/daemons.html#modular-sockets).

## Running Linux guest follow-up

On 2026-11-15, six checks passed with a real Linux/KVM guest using qcow2 in both
normal and ASan/UBSan backend clients. A raw-disk control also passed all six.
The updated empty-session activation test still passed five checks per build,
and both default headless suites passed fourteen checks. No production code or
retry delay changed.

The guest boots the host's Linux kernel with built-in virtio/ext4 support and a
small freestanding QA initramfs. It has one vCPU, 512 MiB RAM, no networking or
graphics, and a private 64 MiB virtio disk. Its PID 1 writes a counter, complement
and magic value to ext4 with O_DSYNC and fdatasync, then reports the synced
counter over its serial PTY every 250 ms. This is a controlled Linux filesystem
workload, not a full Fedora desktop or guest-throughput benchmark.

The test verifies:

1. Linux boots and synced heartbeats advance.
2. A graceful daemon stop leaves the same QEMU PID/start time and guest boot
   instance running, with writes continuing while the daemon is absent and
   after automatic backend recovery.
3. SIGKILL/replacement of that daemon preserves the same properties.
4. The recovered production backend pauses and resumes the same guest UUID.
5. The guest fsyncs, unmounts ext4 and powers off. Only after QEMU exits does
   the runner check qcow2 integrity, inspect a converted raw copy with e2fsck
   and compare the on-disk heartbeat with the guest's final synced record.
6. Booting that disk again loads the saved counter, advances it, then repeats
   clean shutdown and offline integrity checks.

Guest setup/cleanup uses a second virsh client in the private session; Pirwa's
commands, inventory and recovery use its unchanged worker/C API. The supervisor
never restarts a host service. All resources use a generated UUID and private
XDG directories. Both explicit daemon and KVM opt-ins are required. A pidfd
binds emergency cleanup to the exact validated QEMU process. Accepted runs
needed no forced process cleanup and ended with no domain/storage definitions,
private processes or temporary root.

A separate SIGTERM control interrupted the supervisor while the daemon was down
and the guest was still running. Cleanup reactivated the private daemon, stopped
and undefined the guest, joined the backend, confirmed QEMU exit and removed the
runtime. It is recorded as an expected interrupted run, not a completed matrix.
The first exploratory run rejected a successful Start reply because the harness
incorrectly required connection metadata on every mutation reply. That assertion
was limited to recovery inventory, where the production API supplies metadata;
it was not an application failure.

The [guest validation record](guest-validation.json) includes frozen build source
and binary hashes, all checkpoints, guest/daemon/client logs, offline check
results, opt-in guards and interruption cleanup. ASan/UBSan applies to the
backend client, not to QEMU, the guest kernel or its freestanding init. Leak
detection remains disabled for the existing third-party initialization findings.
The normal and sanitizer guest passes are linked to the frozen Fedora native
build manifest and matching binary/schema hashes. The raw and interruption
controls used earlier pinned clients. Concurrent working-tree changes are
recorded separately; the final combined candidate still needs release validation.

```sh
meson compile -C build test-backend-daemon
PIRWA_RUN_DAEMON_TESTS=1 PIRWA_RUN_KVM_TESTS=1 ./scripts/test-guest-recovery.py \
  --build build --output build/qa/guest-recovery-new
```

Use `--disk-format raw` for the control. See the
[setup requirements](../../implementation/setup.md) for kernel features,
tools and memory headroom. These results do not cover host power loss, a crashed
guest kernel, UEFI/TPM/snapshots/backing chains, other guest filesystems, SPICE
continuity, installed systemd units or system authorization. Those results must
not be inferred from this fixture. Performance gates remain separate.

References: [Linux initramfs](https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html),
[direct kernel boot](https://libvirt.org/formatdomain.html#direct-kernel-boot).
