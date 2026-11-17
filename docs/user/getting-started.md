# Using Pirwa

Build and launch with the README commands. Open the sidebar's main menu and use
**Connection** to select your per-user Session or the System libvirt connection.
These show different inventories.
Normal libvirt/polkit authorization applies; never run Pirwa as root.

## Use the host proxy automatically

**Preferences → Network → Use host proxy** is enabled by default for **Session**
machines. When GNOME has a manual SOCKS or HTTP proxy configured, a machine
started in Pirwa uses it without proxy settings or extra software inside the
guest. A proxy listening on the host's `127.0.0.1` works. The guest uses ordinary
DHCP on a private network. The saved VM definition stays unchanged.

Fully shut down and start the machine to apply a preference or host proxy change.
Already-running machines keep their current network. For example, after setting
the host proxy, start a Session machine and open a website in its browser; there
is no proxy setup step in the guest. Closing Pirwa's windows leaves the network
gateway running until the VM stops. Turning the preference off restores the
machine's usual networking on its next start. With the desktop proxy disabled,
Pirwa uses the usual networking directly.

Proxy routing supports a single ordinary userspace, bridge or virtual network
adapter, including older Boxes machines attached to `virbr0`. While using the
proxy, the guest uses the private DHCP network instead of its usual bridge or
virtual network. Its adapter model, MAC address and saved network configuration
are preserved. Guests configured with a static IP may need DHCP enabled inside
the guest for this mode.

This version supports IPv4 TCP, including HTTPS without decrypting it, and DNS.
SOCKS5 takes priority over HTTP settings. HTTP proxies must allow CONNECT to the
requested destination ports. UDP DNS is carried through the proxy over TCP;
the default resolver is `1.1.1.1:53`. A proxy that denies that port prevents DNS
resolution. To select another IPv4 resolver on the host:

```sh
gsettings set io.github.stonega.Pirwa proxy-dns-address '9.9.9.9'
```

This command requires an installed Pirwa schema; development builds can pass
`--schemadir build/data` to `gsettings`. Restart the VM afterward. Private DNS
names require a resolver reachable through your proxy. This does not change the
host's DNS settings.

IPv6, other UDP traffic (including QUIC), ICMP, and fragmented IPv4 packets are
blocked in proxy mode. Applications that require them may not work. TCP clients
can use their normal fallback where available. PAC configuration, System
connections, multiple network interfaces, and custom routes, filters or port
forwarding are outside this mode. Unsupported Session configurations report a
start error with the full explanation and a **Preferences** button so you can
turn off **Network → Use host proxy** and retry with the usual network.
The host's IP/CIDR proxy exclusions
still allow direct connections; hostname exclusions cannot be matched to raw IP
traffic. A failed proxy connection never falls back to a direct connection.

## Import from Boxes Flatpak

Native GNOME Boxes machines already appear automatically in **Session**. No
import is needed for them. The one-time import prompt appears only when the
stable **org.gnome.Boxes Flatpak** is installed, checking both user and system
installations. A native executable or leftover Flatpak data does not trigger it.
Choose **Not Now** to dismiss it; use **Import from Boxes Flatpak** in the main
menu to open it again.

You can control your existing machines or create a new one while Pirwa checks
the Boxes library. If another dialog is open, the import guide waits until you
close it.

For a Flatpak machine, open Boxes and fully shut down the guest. Keep Boxes open
so its private libvirt service remains available. In Pirwa choose **Refresh
Library**, select the machine and choose **Import Machine**. Do not start or edit
the original until import completes.

Pirwa creates a separate disk copy in its Session storage and copies the guest's
UEFI loader and variable store when present. Allow enough free space for the
copy; large images take time. The dialog shows progress and **Cancel Import**
rolls back the copy. An in-flight libvirt call may need to finish before cleanup.
Source definitions and files are never modified or removed.

The imported machine has a new UUID, network MAC and a name ending in
**(Imported …)**. CPU, memory, guest hardware and OS metadata are retained; the
installation CD is ejected and local display sockets are allocated by the host.
The copy appears stopped in **Session**. Choose **Start** to check it. It runs
without Boxes, including after Boxes is uninstalled. Keep the original until
you have checked the copy; remove the original in Boxes when you no longer need
it. The copied guest OS still has its original internal identity and accounts.

This importer supports one standalone, unencrypted qcow2 disk and user
networking. Guests with snapshots, saved memory, backing chains, TPM, shared
folders, extra disks, passthrough or custom host dependencies are rejected before
copying. Host QEMU compatibility is still required. Stable `org.gnome.Boxes` is
supported; nightly app IDs and remote/custom libraries are outside this workflow.
There is no automatic connection to the Flatpak library after import.

For a standalone exported qcow2 image, use Create/Import below instead.

## Control machines

Pirwa remembers your last selected machine separately for each connection and
selects it when you reopen the app. If there is no saved selection or that machine
is no longer available, it selects the first machine. Selecting a powered-off
machine leaves it stopped until you choose **Start**.

Machine rows show the operating system name when it is recorded in their
metadata, with the state and VM name underneath. If the OS is unknown, the
title uses the VM name. Running status lines appear in green.
Click the sidebar header's Search button to reveal the
search bar, which is hidden by default. Search matches either name. Click Search
again or press Escape to hide the bar. Refresh rechecks OS metadata.
Closing search returns keyboard focus to the machine list so you can continue
with the arrow keys.

Select a running SPICE VM to open its viewer in the content panel automatically.
Stopped machines show their name and allocated resources, with **Start**
centered below them with a power icon beside its label. The center panel hides
Powered off and shows Crashed when applicable.
The stopped view has no display hint, header machine buttons,
or header resource readings. Once running, the content header shows controls on
the left, including **Shut Down**, and resource readings on the right. Paused
machines also show their state in the header.
While Start is pending, a bar in the theme's accent color pulses below the
machine name in the content panel, without a working label or percentage.
Shut Down and Restart are hidden when the machine is not running. Pause/Resume
remain compact icon buttons. When a CD/DVD is mounted, a button after Shut Down
shows its shortened filename and an eject icon. Hover to see the full filename;
click to eject it from the running guest and its next boot. The button also works
while paused, and disappears when no media remains or the machine is stopped. If several
discs are mounted, the next disc appears after ejection. Add, Search,
and the main menu fill the sidebar header. The three-dot
button appears when you hover over a row, select it, or move keyboard focus into
it. It opens **Settings**, **Remove**, and **Force Stop** for that VM.
Power controls ask libvirt to perform the operation. Shut Down depends on
guest cooperation. Pirwa never automatically
turns a slow shutdown into Force Stop. Force Stop has a separate confirmation.

While running or paused, the header uses one row, with controls on the left and the selected VM's configured
CPU count and RAM in a compact group on the right. For a selected running VM,
live CPU usage, resident memory (RSS), and network usage join this group. The
Running and Powered off labels are hidden; the stopped view shows Crashed in the center.
There is no bottom bar. Hover over a
shortened reading to see its full value. CPU 100% means one host CPU;
multi-core guests can exceed 100%. RSS is the VM process's physical memory use,
including overhead, rather than its configured RAM or guest-reported free memory.
Network usage shows receive (↓) and transmit (↑) rates in bytes per second,
summed across the VM's interfaces. Rates need two samples; the initial reading
shows “NET …”. Connections that do not report network counters show
“NET unavailable”.
Values update about every two seconds while the main window is visible, including
when a popped-out viewer has focus. Sampling pauses during dialogs and power
operations, and when the window is hidden or suspended by the desktop.
Unavailable statistics are labeled accordingly; Refresh retries a failed sample. Stopped and paused machines do not show live usage.

The native GTK 4 viewer supports SPICE machines, including Boxes machines.
Choose **Open in Separate Window** in the title bar to pop out the live viewer.
The content panel keeps a screenshot of the display and shows **Bring Viewer
Back** below “Viewer is in a separate window” in the center. That button returns
the same live session to the panel.
The popup fits the guest display and its title bar, scaling down proportionally
when needed to fit your screen. Changing the guest resolution refits the popup;
maximized and fullscreen windows keep their state. You can still resize it by hand.
Closing the popup also returns it while its machine is selected. Selecting
another machine leaves explicit popups open; returning to that machine shows
its preview and return button again. Closing the main window leaves popups running.

Click the display to send input; press Ctrl+Alt or Release Input to return to
the host. Click the display again to recapture. GNOME may ask to allow shortcut
inhibition. The popup header button toggles fullscreen, where the header
automatically hides and the guest display has square corners. Move the pointer
to the top edge to reveal its controls;
move back into the display to hide them. Ctrl+Alt also reveals the header when
releasing input in the popup. There is no viewer bottom status bar. A SPICE guest
agent sets the initial display to match the resolution of the monitor containing
Pirwa, including its display scale (up to 8192 pixels per axis). Resizing the app or switching
to fullscreen scales the image without changing the guest resolution. Use the
guest's Display settings to choose a different resolution or aspect ratio; Pirwa
keeps that choice for the current console, including pop-out and reconnects.
Opening a new console starts with the default again. **Preferences → Viewer →
Automatically adjust guest resolution** restores resolution changes to match the
viewer size. It is off by default, applies to open viewers immediately, and is
remembered between launches. Guests without an agent keep their existing
resolution. Audio playback is enabled. Closing a viewer leaves the VM running. The viewer
connects automatically when you select a running machine and reconnects when a
stopped VM starts again. If the display is temporarily unavailable or its first
frame does not arrive, the viewer retries automatically while the VM is running.
There is no Reconnect button to press.
VNC and SPICE GL machines use virt-viewer, which must be installed separately.
Only the first guest display is supported. Accelerated GL scanout, clipboard,
microphone and locked relative pointer input are follow-up work.

## Preferences and app information

The main menu's **Preferences** item opens appearance options: System, Light,
and Dark. Appearance changes apply to all Pirwa windows for the current application
session. The Viewer group controls automatic guest resolution adjustment, which
is disabled by default.
**About Pirwa** shows the application version, contributor name, and license.

## Create or import

Choose the + button (New Virtual Machine), then follow the four steps:

1. **Image:** drop a local bootable x86 Linux ISO onto the drop area, or choose
   **Choose an ISO…** to browse for it. The selected filename and path appear
   inside the drop area; use **Choose Another File…** to replace the image.
2. **Name:** keep or edit the name suggested from the image filename, and choose
   a bundled preset under **Machine Icon** if you want to customize its avatar.
3. **Config:** choose the operating system, CPU count, memory, disk capacity, storage pool, and firmware.
4. **Boot:** review the settings and choose **Create and Boot**.

Choose **Continue** at the top right to advance. A gray highlight in the bottom
bar slides to the current step; click an earlier step to return to it. Your edits
are retained. The session
connection can create a private pool named pirwa. System mode requires an
existing pool that your libvirt connection can write to.

**Create and Boot** allocates the disk and boots the VM. Its viewer opens in the content
panel so you can install Linux. The dialog shows progress. Cancel rolls back
newly allocated resources;
an in-flight libvirt call may take time to finish before cancellation completes.

For experimental Windows 10/11 installation, choose the matching operating system
and supply a VirtIO driver ISO. Follow the [Windows installation guide](windows.md)
for firmware prerequisites and loading drivers during setup.

New machines use QXL graphics with the native SPICE viewer, including for the
Fedora COSMIC live desktop. This uses software rendering; 3D acceleration is not
enabled. Existing machines keep their configured graphics device.

After installation finishes, use the filename-and-eject button in the content
header to eject the installation media. The button shows a circular spinner
while removing the disc and becomes available again if removal fails. Eject also
releases media locked by the guest, so finish using the installation media before
ejecting it. You can also shut down the guest, open
**Settings**, enable **Eject installation media**, and choose **Save Changes**.
Shut down and choose **Start** to boot the installed system from its disk and
finish the distribution's first-run setup. This workflow has been exercised with Fedora
Workstation 43; other installers may present different steps.

For an existing standalone qcow2 image, enable **Import an Existing Disk** on
the Image step, then drop or choose the disk image. Pirwa copies it into an
owned volume and uses the image's virtual capacity; the disk-capacity field
does not resize imports. Select legacy BIOS if that is how the image was
installed. Backing chains, encryption, external data files, and incompatible
qcow2 features are rejected. Source images must be stable regular files,
not symlinks, and remain unchanged.

## Edit or remove

Open the machine's three-dot menu and choose **Settings**. Under **Machine Icon**,
open the picker, search or browse the 16 robot expressions, and select a face.
Choose **Save Changes** to keep it, or **Cancel** to discard it. The icon appears
inside a monitor frame in the sidebar and on the stopped-machine screen.
You can change it while the machine runs. Choose **Friendly** to restore the
default. Choices are saved locally for each connection and machine and survive
renames and app restarts. No ISO file or internet connection is needed.
The avatars show simple eyes and mouths inside the monitor frame. While the
machine runs, the preset's colored background fills the entire screen. The
screen clears when paused or stopped. The frame keeps its normal theme color,
and the chosen expression stays the same. Choices from the earlier distro-logo
set display Friendly until you
choose a new expression.

Power off the VM, open its three-dot menu, and choose **Settings**. Rename it,
change CPU/RAM, or eject its
installation ISO. Changes take effect on the next boot. Advanced domain layouts
may require another manager; libvirt errors are shown without discarding
unrelated device configuration.

Remove opens a confirmation. Disks are kept by default. Disk deletion is offered
only when Pirwa can verify its ownership record and domain metadata; it verifies
the actual volume again and checks references from other VMs before deletion.
Do not concurrently reassign disks in another client while deleting a VM.
Losing the ownership record prevents automatic disk deletion.

## Connections and shortcuts

Lifecycle events update the list automatically. After a disconnect Pirwa retries
five times with increasing delays. Refresh reconnects manually and starts a new
retry cycle if the service is still unavailable. Reconnection refreshes the
available storage pools. If a power command fails, check the machine's state
before choosing it again. Connection switching is disabled during an operation.
Ctrl+N opens creation; Ctrl+R or F5
refreshes/reconnects. Ctrl+F opens and focuses search. The collection receives
initial keyboard focus when machines are available.

The test-driver example uses synthetic VMs. Its controls can be exercised without
real guests, but creation and real console access require QEMU/KVM.
