# Fedora installation acceptance — 2026-11-13

This is a real QEMU/KVM installation test, separate from the synthetic UI matrix.
Pirwa created the guest through its production form, Fedora's installer reported
success, Pirwa shut it down, and Settings detached the ISO before disk boot.
This records one guest/host combination, not the complete release matrix.

## Environment and isolation

- Fedora 45 host; GNOME Shell/Mutter 51.1; GTK 4.24.1; libadwaita 1.10;
  libvirt 12.6; spice-gtk 0.43; virt-viewer 11.0-20.fc45.
- Application source: `1af72e99777b51da66fcda2f18a7e76206993d04`.
  Only the desktop test wrapper changed during the run; application code did not.
- A private GNOME Shell Wayland desktop with a 1280×800, 60 Hz virtual monitor,
  private D-Bus/runtime/config/data/cache/state, and memory GSettings.
- Native keyboard events entered the production creation form and navigated
  the installer. Mutter RemoteDesktop and a ScreenCast stream supplied input;
  GNOME Shell Screenshot captured only this private desktop. This is injected
  desktop input, not a physical-device or screen-reader certification.
- An explicitly selected host session socket ran the disposable guest. Its
  name, UUID, pool UUID/path, and the existing guests' XML hashes were recorded
  before changes. All mutations targeted the new guest. The pool was prepared
  with virsh as test setup; Pirwa's implementation still uses libvirt directly.
- The distribution-signed virt-viewer RPM was verified and extracted into the
  temporary development prefix. No system package was installed.

Guest: Fedora Workstation Live 43-1.6 x86_64, four vCPUs, 4096 MiB RAM, a new
32 GiB qcow2 disk, UEFI, virtio devices, userspace networking, and local SPICE.
The local ISO's SHA-256 is
`2a4a16c009244eb5ab2198700eb04103793b62407e8596f30a3e0cc8ac294d77`;
this identifies the tested file and is not an upstream checksum attestation.

## Desktop fixture correction

The minimal headless desktop initially lacked audio policy services. The viewer
stalled at “Connected to graphic server” while libvirt screenshots showed a live
Fedora desktop. A diagnostic viewer with audio disabled established video and
keyboard operation. Audio probing could still delay those diagnostic launches.

Adding a private PipeWire Pulse server, a null sink, and WirePlumber's `policy`
profile resolved the fixture's connection stall. The null sink and its monitor
were the selected defaults; hardware monitor profiles were not enabled.
Pirwa then opened its normal viewer command promptly, with no audio-disable flag:

```text
virt-viewer --connect <session URI> --attach --reconnect --wait <test UUID>
```

The remaining installer steps and disk boot used that production launch path.
This does not verify audible output, microphone forwarding, or audio latency.
The temporary audio processes were scoped to the private runtime.

## Observed workflow

1. Enter the name, CPU/RAM, ISO, and selected disposable pool in Pirwa; Create
   allocates the owned disk and starts the VM. [Creation form](create-form.png).
2. Open Console and run Fedora's graphical installer with English (US), the
   entire new virtio disk, automatic partitioning, and no disk encryption.
   [Installer success](installed.png).
3. Exercise fullscreen and return to the window; use Pirwa's Shut Down action
   and wait for the powered-off state.
4. Open Settings, select Eject installation media, and save.
   [Eject selection](eject-iso.png). Read-only XML inspection confirms that the
   CD source and CD boot order are absent.
5. Start through Pirwa and reopen Console. Fedora boots from the disk into its
   first-run setup. Complete a disposable local account and enter the installed
   GNOME desktop. [First boot complete](first-boot.png).
6. In the guest terminal, `uname -r`, `findmnt -no SOURCE,FSTYPE /`, and
   `/etc/fedora-release` confirm kernel `6.17.1-300.fc43.x86_64`, Fedora 43,
   and `/dev/vda3[/root] btrfs`. An IPv4 default route is present on the virtio
   interface. [Installed desktop and disk evidence](disk-desktop.png).
7. Shut down through Pirwa, remove the guest with its verified owned disk, and
   verify the pool is empty before destroying/undefining that test pool.
   Both existing Boxes definitions retain their original hashes and stopped
   states. [Machine-readable result and baseline comparison](report.json).

The custom controller and compositor exited successfully. The outer launcher
ended with SIGTERM during teardown and left its private temporary directory;
no private processes or mounts remained. That directory was explicitly checked
and removed. This exploratory launch is therefore not an automated teardown
reliability sign-off. The subsequent five-case regression run used the current
wrapper and exited normally; its [raw report](ui-regression.json) is retained.

## Repeating the check

Build Pirwa and install its normal runtime dependencies using the setup guide.
On a GNOME Wayland desktop, record a baseline of existing VM definitions/states,
create a uniquely named disposable directory pool, and follow the steps above
with a local Fedora ISO. Keep the test VM's UUID and owned disk path recorded.
After checking the installed desktop, shut down, remove that guest with its
verified Pirwa disk, and remove only the now-empty test pool. Compare the
existing definitions/states against the baseline.

For a custom virtual-desktop controller, the wrapper supports:

```sh
./scripts/test-gnome.py --command-timeout 7200 \
  --output build/qa/linux-install/session -- <controller-command>
```

The wrapper supplies isolation and a deadline. The controller must supply its
own input, screenshots, private audio fixture, guest identity checks, and
cleanup. It must never use an existing guest as the disposable test target.
This run's raw exploratory controller, compositor/viewer logs, manifest, XML,
and screenshots are under `build/qa/linux-install/` and are not packaged.

## Limits

One Fedora guest is covered. This is not guest-throughput, audio, clipboard,
physical input, accessibility, multi-monitor, or multi-distribution acceptance.
The default Meson checks and five-case virtual GNOME UI matrix remain synthetic;
both passed again during this run. No performance improvement is claimed.
