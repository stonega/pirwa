# Using Pirwa

Build and launch with the README commands. The header selects your per-user
Session or the System libvirt connection. These show different inventories.
Normal libvirt/polkit authorization applies; never run Pirwa as root.

## Bring your GNOME Boxes machines

On the first eligible launch, Pirwa checks whether GNOME Boxes is installed and
its local libraries contain machines. It opens **Import from GNOME Boxes** with
the detected machines and the library they belong to. Choose **Use Machines**
to connect and select a machine, or **Not Now** to continue. Dismissing the guide
is remembered. The folder button in the header (**Import from GNOME Boxes**)
opens it again whenever needed.

The regular GNOME Boxes package shares your per-user libvirt Session library
with Pirwa. Its machines are already accessible there. Import uses the existing
VMs directly: it does not duplicate disks, redefine devices, move files, or
transfer ownership. Both applications control the same machines. Boxes disks
remain protected by Pirwa's existing ownership checks when removing a VM.

For **Boxes Flatpak**, choose that library in the guide. If it is offline, choose
**Open GNOME Boxes**, then **Refresh Library**. Keep Boxes running so its private
libvirt service remains available. **Use Machines** adds the Boxes Flatpak
connection to the header and remembers it for the next launch. If that service
is unavailable later, open Boxes and refresh/reconnect, or choose Session.
Stable `org.gnome.Boxes` is supported; nightly app IDs and remote/custom Boxes
connections are outside this workflow. Flatpak VM definitions are inspected
read-only; disks are never scanned or copied during discovery.

This workflow gives Pirwa access to the existing libraries. Creating an
independent VM from an exported standalone qcow2 image uses Create/Import below.

## Control machines

Select a VM to see its state, allocated CPUs/RAM, and controls. Search filters
names. Start, Shut Down, Restart, Pause, and Resume ask libvirt to perform the
operation. Shut Down depends on guest cooperation. Pirwa never automatically
turns a slow shutdown into Force Stop. Force Stop has a separate confirmation.

Open Console launches virt-viewer in another window. Install virt-viewer first.
Console input, fullscreen, and guest resize are handled by the viewer. Guest
agents may be needed for the best resize/integration experience.

## Create or import

Choose the + button (New Virtual Machine). Enter a name, CPU count, memory, disk capacity,
local bootable x86 Linux ISO, and active directory storage pool. The session
connection can create a private pool named pirwa. System mode requires an
existing pool that your libvirt connection can write to.

Create allocates the disk and boots the VM. Open Console to install
Linux. The dialog shows progress. Cancel rolls back newly allocated resources;
an in-flight libvirt call may take time to finish before cancellation completes.

For an existing standalone qcow2 image, enable Import. Pirwa copies it into an
owned volume and uses the image's virtual capacity; the disk-capacity field
does not resize imports. Select legacy BIOS if that is how the image was
installed. Backing chains, encryption, external data files, and incompatible
qcow2 features are rejected. Source images must be stable regular files,
not symlinks, and remain unchanged.

## Edit or remove

Power off the VM before Settings. Rename it, change CPU/RAM, or eject its
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
refreshes/reconnects. Ctrl+F focuses search. The collection receives initial
keyboard focus when machines are available.

The test-driver example uses synthetic VMs. Its controls can be exercised without
real guests, but creation and real console access require QEMU/KVM.
