# Using Pirwa

Build and launch with the README commands. Open the sidebar's main menu and use
**Connection** to select your per-user Session or the System libvirt connection.
These show different inventories.
Normal libvirt/polkit authorization applies; never run Pirwa as root.

## Import from Boxes Flatpak

Native GNOME Boxes machines already appear automatically in **Session**. No
import is needed for them. The one-time import prompt appears only when the
stable **org.gnome.Boxes Flatpak** is installed, checking both user and system
installations. A native executable or leftover Flatpak data does not trigger it.
Choose **Not Now** to dismiss it; use **Import from Boxes Flatpak** in the main
menu to open it again.

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

Machine rows show the operating system name when it is recorded in their
metadata, with the state and VM name underneath. If the OS is unknown, the
title uses the VM name. Click the sidebar header's Search button to reveal the
search bar, which is hidden by default. Search matches either name. Click Search
again or press Escape to hide the bar. Refresh rechecks OS metadata.

Select a running SPICE VM to open its viewer in the content panel automatically.
Stopped machines show their state and allocated resources. The content header
shows controls on the left and the current state on the right. **Start**
is the main button for a stopped machine; **Shut Down** replaces it while running.
Shut Down and Restart are hidden when the machine is not running. Pause/Resume
remain compact icon buttons. Add, Search,
and the main menu fill the sidebar header. The three-dot
button on each row opens **Settings**, **Remove**, and **Force Stop** for that VM.
Power controls ask libvirt to perform the operation. Shut Down depends on
guest cooperation. Pirwa never automatically
turns a slow shutdown into Force Stop. Force Stop has a separate confirmation.

The header uses one row, with controls on the left and the host's architecture,
CPU count, total RAM, and VM state on the right. For a selected running VM,
live CPU usage, resident memory (RSS), and network usage join this right-hand group.
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
Back** at the top center. That button returns the same live session to the panel.
Closing the popup also returns it while its machine is selected. Selecting
another machine leaves explicit popups open; returning to that machine shows
its preview and return button again. Closing the main window leaves popups running.

Click the display to send input; press Ctrl+Alt or Release Input to return to
the host. Click the display again to recapture. GNOME may ask to allow shortcut
inhibition. The popup header button toggles fullscreen, where the header
automatically hides. Move the pointer to the top edge to reveal its controls;
move back into the display to hide them. Ctrl+Alt also reveals the header when
releasing input in the popup. There is no viewer bottom status bar. A SPICE guest
agent enables automatic resize; otherwise the display scales to fit. Audio
playback is enabled. Closing a viewer leaves the VM running. The viewer
reconnects when a stopped VM starts again, and offers Reconnect after an error.
VNC and SPICE GL machines use virt-viewer, which must be installed separately.
Only the first guest display is supported. Accelerated GL scanout, clipboard,
microphone and locked relative pointer input are follow-up work.

## Preferences and app information

The main menu's **Preferences** item opens appearance options: System, Light,
and Dark. Changes apply to all Pirwa windows for the current application session.
**About Pirwa** shows the application version, contributor name, and license.

## Create or import

Choose the + button (New Virtual Machine). Enter a name, CPU count, memory, disk capacity,
local bootable x86 Linux ISO, and active directory storage pool. The session
connection can create a private pool named pirwa. System mode requires an
existing pool that your libvirt connection can write to.

Create allocates the disk and boots the VM. Its viewer opens in the content
panel so you can install Linux. The dialog shows progress. Cancel rolls back
newly allocated resources;
an in-flight libvirt call may take time to finish before cancellation completes.

After installation finishes, shut down the guest. Open **Settings**, enable
**Eject installation media**, and choose **Save Changes**. Then choose **Start**
to boot the installed system from its disk and finish the distribution's first-run
setup. This workflow has been exercised with Fedora
Workstation 43; other installers may present different steps.

For an existing standalone qcow2 image, enable Import. Pirwa copies it into an
owned volume and uses the image's virtual capacity; the disk-capacity field
does not resize imports. Select legacy BIOS if that is how the image was
installed. Backing chains, encryption, external data files, and incompatible
qcow2 features are rejected. Source images must be stable regular files,
not symlinks, and remain unchanged.

## Edit or remove

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
five times with increasing delays. Refresh reconnects manually. Connection
switching is disabled during an operation. Ctrl+N opens creation; Ctrl+R or F5
refreshes/reconnects. Ctrl+F opens and focuses search. The collection receives
initial keyboard focus when machines are available.

The test-driver example uses synthetic VMs. Its controls can be exercised without
real guests, but creation and real console access require QEMU/KVM.
