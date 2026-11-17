# Architecture

## Runtime

One native C17 desktop executable uses GTK 4/libadwaita on the main thread.
Its application, desktop, icon, AppStream and GSettings schema ID is
`io.github.stonega.Pirwa`, using the repository owner's GitHub namespace.
GSettings retains the pre-release `/io/github/pirwa/Pirwa/` storage path so
proxy, display and per-connection selection preferences remain available without
startup migration writes. The old schema ID exists only in an uninstalled test
fixture. VM ownership records continue to use their established data paths.
A persistent backend owns one selected libvirt connection and one worker queue.
All connection, domain, storage, media, and filesystem operations run on that
worker. Libvirt-glib registers event sources with the GTK/GLib main loop before
the connection opens. Event callbacks only enqueue coalesced reconciliation;
they do not make blocking libvirt calls.

    main.c → pirwa-window.c → pirwa-backend.c
                 │                ├─ pirwa-inventory.c: one bulk state query
                 │                ├─ pirwa-config.c: escaped XML and image checks
                 │                └─ pirwa-provision.c: storage, media, ownership
                 └─ pirwa-model.c: ordered, stable snapshot reconciliation

Libvirt manages QEMU/KVM. Pirwa does not launch QEMU itself or call virsh in
application logic. SPICE consoles use `pirwa-console.c` (GTK 4/libadwaita and
SPICE GLib) and `pirwa-frame.c` (immutable textures and input coordinate mapping).
Each open console owns a backend worker scoped to its connection URI and UUID;
its authenticated per-channel sockets come from `virDomainOpenGraphicsFD` on
that worker. Reply-owned FD lists close discarded results. Generation fences
reject sockets from previous sessions. The selected running SPICE machine opens
as a reusable console widget in the content panel. Pop-out and return reparent
that widget without restarting its transport. A single immutable frame supplies
the detached preview, with no screenshot timer. Explicit popups survive closing
the collection; inline sessions close on selection changes or collection closure.
VNC and SPICE GL use virt-viewer with
an argument array, connection URI, and domain UUID.

## Model and work queue

Workers produce owned snapshots. The main thread reconciles them against
canonical GObjects identified by connection and UUID. State changes update
existing objects; list insertions/removals replace only the differing range.
Startup and connection switches show a spinner without a connection-status
title. The indicator retains an accessible label for screen readers.
GtkListView recycles rows and a filter model searches VM and OS names. Each row
uses the OS name from libosinfo metadata when known, with its state and VM name
underneath; missing or unknown metadata falls back to the VM name. The backend
reads OS metadata once per discovered UUID and caches missing values too.
Redefining a VM, refreshing manually, or reconnecting invalidates this cache;
removed UUIDs are pruned. The libosinfo database loads lazily on the worker.
Full domain XML and storage details are loaded only for the selected VM.
After loading an inventory, the window keeps its current selection when possible.
Otherwise it restores the last selected UUID for that connection, falling back to
the first machine when the saved UUID is absent. An empty inventory stays unselected.
GSettings stores one selected UUID per libvirt URI; selection changes save it
without adding synchronous filesystem work. Clearing selection or a search with
no matches does not erase the saved choice. Restoration uses UUIDs rather than row positions,
so renaming or reordering machines preserves the choice. Selecting a stopped
machine does not start it.
Machine avatars use a bundled, searchable catalog of 16 robot expressions. The sidebar,
stopped-machine view and Settings preview compose the same theme-aware monitor
frame around the chosen eyes and mouth. While running, the preset's colored
background fills the screen to the bezel, with dark facial features for contrast.
The frame always uses the normal theme foreground; it has no active accent tint.
Paused and stopped machines have a transparent screen with normal foreground
features. A shared screen mask sits beneath the frame and keeps its layout at
zero opacity when inactive. Existing VM events update the fill without polling.
Picker previews stay transparent. The gallery is built on its first expansion.
SVG assets are embedded in GResource; no ISO
inspection, download, background scan or new runtime dependency is involved.
GSettings stores choices by connection URI and UUID in `machine-avatars`,
independently of libvirt definitions. Missing or unknown preset identifiers use
the Friendly face. Friendly retains the `generic` preset identifier and removes
the stored override when chosen. Old distro identifiers also display Friendly
without rewriting preferences during discovery. Expressions remain stable
across power-state changes and use no animation or timers.
Recycled rows bind the connection and UUID afresh and observe preference changes,
so a choice updates open windows without an inventory refresh.
Settings can change an icon while the machine runs; name and resource controls
remain disabled until it is stopped. Cancel discards the choice. An icon-only
save writes only local preferences. When name/resources are also changed, the
icon is committed after the backend edit succeeds.
The creation dialog shares the picker in its Name step and previews the framed
icon in its final summary. Its choice is saved only after successful creation,
under the new UUID; failed or cancelled creation stores no avatar preference.
The search field is hidden until the sidebar's Search button or Ctrl+F opens it;
the button toggles it and Escape closes it. The search field and machine rows use
matching horizontal insets. The sidebar remains resizable without a visible
divider between the list and details.
The search bar uses libadwaita's inline style so its surrounding area blends
with the sidebar in light, dark, and inactive-window states.
Its header and collection use libadwaita's sidebar background and foreground
colors, including the inactive-window variant, in both light and dark themes.
Remove in the machine options menu uses the theme's destructive text color.
Each pane owns its header, so Add, Search, and the main menu span the sidebar
width, while machine controls sit at the left of the
content header while a machine is running or paused. A powered-off or crashed
machine shows Start centered below its name and allocated resources, without a
display hint. The center panel hides Powered off and retains Crashed when applicable.
Its header hides machine controls and readings while
retaining the standard window buttons. This status view also replaces an existing
inline console when the selected machine stops.
The selected VM's configured resources and live usage share a compact group on the
right. Each reading sizes to its text with a small gap between readings and
ellipsizes when space is limited; the selected machine's state appears there only
for non-stopped states other than Running.
Start in the status view includes a power icon beside its label. Its progress
bar is centered at a compact 150 logical pixels wide. Start and
Shut Down use prominent text buttons;
Shut Down uses the theme's warning background and foreground colors.
When a CD/DVD has media, a button immediately after Shut Down shows its filename
and an eject icon. Its label ellipsizes to an 18-character width; the tooltip and
accessible label retain the full filename. It is also available while paused
and hides when the tray is empty or the machine is stopped. With several mounted discs, it
shows the first one, then the next after ejection. While ejecting, a circular
spinner replaces the eject icon inside the disabled button, retaining its size
and filename. The tooltip and accessible label report ejection in progress, and
the button exposes its busy state. Ejection does not show the global progress row.
A toast confirms successful media ejection after the backend operation completes.
The worker checks the displayed source and target against fresh XML before
ejecting, updates live and persistent
configuration where applicable, and uses `VIR_DOMAIN_DEVICE_MODIFY_FORCE` for a
live eject so a locked installer tray cannot block removal. Stopped machines
update only their saved definition without the force flag. Tray events
refresh selected details without polling. Resources continue to come from the
saved definition while the displayed media follows the live tray.
The eject XML removes the media source and preserves the drive's target
attributes, including its tray state. QEMU rejects a live update that explicitly
changes the tray field; libvirt handles opening the tray when removing media.
Controls, readings, and the standard GTK window buttons share one horizontal
box in the header's title area. Including the window buttons in this row avoids
a mirrored centering gap at the left inset in wide windows. Controls and readings
shrink together without overlapping. Text buttons can ellipsize at narrow
widths and retain their full tooltip and accessible label.
Shut Down and Restart
are visible only while the machine is running. Header heights stay aligned as
the sidebar is resized.
The connection selector is a custom row inside the main menu and retains the
same connection-switching and Boxes library logic. Preferences and About use
native libadwaita dialogs. Appearance preferences update the shared style
manager for the current application session.
Automatic guest resolution adjustment is a persisted GSettings preference,
disabled by default. Consoles observe changes directly, including detached ones.
With it off, the initial mode matches the host window's monitor in device pixels,
including its display scale, bounded to 8192 pixels per axis. Selection waits for
the console to be mapped; subsequent resizing uses local aspect-preserving
scaling and respects guest-selected modes. SPICE monitor geometry excludes QXL allocation
padding from rendered pixels, input mapping, and preview snapshots.
The existing application SVG is embedded as a GResource so native About dialogs
can show it in both development builds and installed copies.
Each row's three-dot menu selects that VM and carries its UUID for Settings,
Remove, and Force Stop. A stale target cannot act on a different selection.
The row stores only the bound UUID until its menu is first opened; GTK's
create-popup callback then builds the model and popover. Unbinding closes and
discards the menu and UUID, so recycled rows cannot retain another VM's targets.
The menu appears when the row is hovered, selected, or contains keyboard focus.
Opacity keeps its space reserved so labels do not shift when it appears.

One foreground command runs at a time in the UI. The worker serializes commands
and reads; lifecycle refreshes are coalesced. Selecting another VM cancels old
detail requests. Switching connections uses a new backend, so late callbacks
from the old backend are discarded.

Each backend completion has a monotonically increasing serial. Snapshot and
connection-error delivery use the same ordering fence, so an older callback
cannot replace newer state. Selected-detail requests have a separate generation
counter, including when the same UUID is selected again. Settings fetches fresh
details before opening. If an inventory refresh replaces that read for the same
VM, the replacement keeps the explicit request to open Settings. Changing or
clearing selection, switching connections, closing the window, or completing
the current read clears that intent; an obsolete completion cannot clear a
newer request or open a dialog for another selection.
Progress carries the foreground request ID; queued
progress from an earlier job cannot update a later job's dialog.

The right pane has no bottom bar. Its single-row header shows controls on the
left, with the selected VM's configured CPU count and RAM, live CPU usage, resident
memory (RSS), and network receive/transmit rates grouped on the right. Configured
resources come from the selected domain's details and clear when selection changes;
the header shows a loading message while those details are fetched. Host hardware
and architecture are not shown. Running and Powered off labels are hidden; other VM
states remain visible.
Usage appears while the VM is
running. Long readings ellipsize, with full values in
tooltips. Starting a machine shows only a pulsing progress bar in the theme's
accent color directly below its name in the content panel. It follows the
selected machine and stops when the start request finishes. Connection changes
and refreshes do not show the foreground progress strip. Other foreground
operations retain the temporary progress and Cancel strip below the header.
One read-only bulk libvirt sample runs on the backend worker
at most once every two seconds, only while the main window is mapped and not
suspended by the compositor, without a modal dialog or foreground operation. At most one sample is in flight.
Selection/connection changes cancel pending work and fence late results. Hiding,
suspending, pausing, or stopping cancels sampling; suspended windows retain the
last displayed value. CPU percentages use cumulative CPU time and monotonic
elapsed time; 100% represents one host CPU. RSS includes VM-process overhead.
Network byte counters come from the same bulk sample and are summed across all
reported VM interfaces. Rates use monotonic elapsed time; the first sample,
interface-name changes, and decreasing totals establish a new baseline. Missing
interface counters or overflowing totals make network usage unavailable without
discarding CPU or RAM readings.
Missing fields remain unavailable. Failed sampling waits for a selection change
or manual refresh rather than repeatedly reporting errors.

There are no per-VM collection screenshot or stats timers. A disconnected backend
tries reconnect at 1/2/4/8/16-second intervals, then waits for manual reconnect.
QEMU close notifications retain callback data until deregistration. The test
driver has no persistent transport and skips its successful no-op close
registration, which does not release opaque data.
Cancelled queued work does not consume connection attempts. Manual reconnect
starts a fresh retry cycle. Opening a replacement connection refreshes host and
storage-pool information as well as the inventory, including automatic recovery.
Mutations are never replayed automatically. Unselected inventories have no
polling or retry timer while connected.

## Boxes Flatpak discovery and import

`pirwa-boxes.c` runs cancellable discovery away from GTK. It uses `flatpak info
--show-ref org.gnome.Boxes` to check the installed application in user and system
installations. Native desktop entries and residual Flatpak data do not qualify.
Native Session machines use the ordinary inventory; they never appear in the
import dialog. An installed Flatpak can show the introduction even with an empty
library. Dismissing or completing the introduction atomically records it under
`XDG_STATE_HOME/pirwa/boxes-import.ini`; subsequent automatic launches read only
that preference. Old Flatpak connection preferences are no longer restored.

Stable Flatpak definitions are inspected read-only under
`~/.var/app/org.gnome.Boxes/config/libvirt/qemu`. Discovery reads bounded regular
XML files without following final symlinks or external entities, deduplicates
UUIDs, and locates a user-owned runtime socket. Offline libraries offer Open
Boxes and Refresh Library. Discovery never opens disk images or mutates domains.

The native inventory, machine controls and creation remain usable while this
read-only scan runs. Import actions wait for its result. The automatic guide
keeps a pending result while another dialog or foreground operation is open and
is offered after that operation closes.

`pirwa-boxes-dialog.c` explains shutdown, storage space, cancellation, source
retention and supported guest layouts. **Import Machine** submits
`PIRWA_IMPORT_BOXES` to the Session backend worker. The dialog remains open with
progress and cancellation; errors remain visible and allow retry. A foreground
application hold covers the operation and rollback.

`pirwa-import.c` prepares an independent definition, preserving guest hardware
and OS metadata while assigning a new UUID/name/MAC, ejecting installation media,
and clearing source-specific emulator, security labels and display socket paths.
The supported layout is one standalone qcow2 disk with user networking. TPM,
snapshots, backing chains, shared folders, passthrough and custom host dependencies
are rejected. `pirwa_provision_import_boxes` reads the source through a read-only
connection and requires a persistent, fully stopped guest without saved memory.
It checks the disk header, takes a read lock, copies to a new owned volume, and
rechecks power state, XML and file timestamps before registering the copy.

UEFI loader and NVRAM files are copied into a private directory in
`XDG_DATA_HOME/pirwa/firmware`. Flatpak `/app` and `/usr` firmware paths are resolved
against the installed application/runtime deployments. The destination uses the
copied manual firmware loader, so it does not depend on Boxes remaining installed.
Firmware copies reject special files, are bounded to 64 MiB and use new 0600
files. The existing volume ownership ledger and rollback protections apply to the
new disk. Failed or cancelled imports clean up new resources, preserving evidence
when cleanup cannot safely finish. Successful copies remain stopped for the user
to check with Start; the source is never modified or deleted. Copied loader files
are retained as firmware assets when a successfully imported VM is later removed.

## Cancellation and shutdown

Caller cancellation and backend shutdown feed a combined worker cancellable,
including requests submitted without a caller cancellable. Cancellation is
checked before operations, during pre-removal reference scans, and between
image upload chunks and partial stream sends.
It cannot interrupt an in-flight synchronous libvirt RPC. After a mutation
commits, the result reports success rather than a misleading cancellation.
Closing the actual window cancels work, stops the queue, and prevents late UI access.
With a modal dialog open, a window close request first closes that dialog and
keeps the backend alive. A busy dialog can reject closing until its operation
is cancelled. Disposal always cancels the window's work independently.
Foreground requests hold GApplication until completion, so closing the last
window does not terminate a worker in the middle of provisioning rollback.
The backend thread cleans up independently without blocking GTK on join.

## Provisioning and ownership

`pirwa-create-dialog.c` provides Image, Name, Configuration, and Boot pages in a
native libadwaita dialog. Page headings retain the desktop font and `title-1` size
with bold weight (700), avoiding the crowded Latin lettering produced by the
default extra-bold weight with Noto Sans CJK HK. See the UI rules in `AGENTS.md`.
A bottom step bar highlights the current page in gray
and lets users return directly to earlier pages; later pages require Continue so
validation cannot be skipped. One highlight slides between buttons using a 240 ms
libadwaita animation, starting from its current position when navigation interrupts
it. The step bar uses one native layout and snapshots the highlight behind its
buttons using their full bounds, keeping it aligned with native hover backgrounds.
It follows the desktop animation setting and the buttons' actual layout, including
right-to-left text and resizing. Continue sits at the top right of the header and
becomes Create and Boot on the review page. The first page accepts one
local ISO through a file picker or file drop, with an explicit qcow2 import mode.
After selection, the filename and full path replace the instructions inside the
drop area, above Choose Another File. Switching import mode restores the prompts.
The image basename supplies an editable default name; changing the source
preserves a custom name.
Step navigation retains all inputs. Configuration requires a storage pool, and
the final review submits one create request through the existing backend queue.
During creation the inputs are disabled, progress remains visible, and Cancel
requests the existing rollback. The content panel uses a centered, 150-pixel
pulsing accent bar matching the machine-start indicator in place of its wide
foreground strip. The dialog retains its progress text and cancellation controls.
Failed creation stops the pulse and keeps the review open for retry.
The dialog checks file extensions without filesystem I/O; source inspection and
bootability checks remain on the backend worker.

The configuration page also offers experimental Windows 10/11 profiles. Selecting
one raises the initial resources to at least 2 CPUs, 4096 MiB and 64 GiB, and
requires a local virtio-win ISO. Both the installation ISO and driver ISO are
attached read-only as separate SATA CD drives; storage and networking remain
VirtIO. The worker validates regular files, ISO structure, architecture, and any
identified OS/profile mismatch before allocating storage. Unknown media can use
the explicitly selected profile. The driver path is copied into the backend job
with the other owned request strings.

Windows 11 additionally requests UEFI with Secure Boot and enrolled keys, SMM,
and a persistent emulated TPM 2.0 using tpm-crb. Domain capabilities gate UEFI and
TPM support before allocation. When available, firmwareFeatures also gates Secure
Boot/key support; older libvirt versions verify the requested firmware combination
during domain definition and use the existing rollback on failure. Libvirt owns
firmware selection and TPM processes/state. Windows uses a local-time hardware
clock. A plain Windows disk import is rejected by these profiles because changing
its storage controller or creating a different TPM can prevent boot or recovery.

Rollback removes newly created TPM state along with firmware variables. Explicit
removal that keeps a TPM-backed disk also keeps its TPM and NVRAM/varstore state;
verified owned-disk deletion removes them. Discovery still leaves existing guest
definitions alone. See the [Windows guide](../user/windows.md) for setup and limits.

The source is opened read-only without following symlinks. ISO media must be
bootable and is identified with libosinfo. Guest defaults use virtio storage and
networking, with QXL graphics over local, non-GL SPICE. Unaccelerated virtio-gpu
fails to start some Wayland compositors, including the Fedora COSMIC 42 live
desktop; QXL allows their software-rendering path to work with the native viewer.
The user explicitly chooses CPU, RAM, and disk capacity. KVM, host-passthrough CPU,
SPICE, and QXL must appear in domain capabilities before storage is allocated.
This graphics default applies to new ISO machines and standalone disk imports;
discovery, resource edits, and Boxes imports preserve existing graphics devices.
UEFI is selected when available unless legacy BIOS is requested. New Linux ISO
machines and standalone disk imports explicitly request `enrolled-keys='no'`,
so unsigned bootloaders can run even when the host prioritizes firmware with
Secure Boot keys. The Secure Boot capability itself remains unspecified, allowing
libvirt to choose either plain firmware or capable firmware with an empty key
store. Legacy BIOS and Windows profiles retain their separate firmware policy;
discovery, resource edits, and Boxes imports preserve existing firmware and keys.
See [libvirt's Secure Boot documentation](https://libvirt.org/kbase/secureboot.html).
Passt is used when supported and installed; the userspace default is the fallback.

Only active directory pools are supported. The session default can create a
private Pirwa pool under the user's data directory. System connections require
an existing pool and normal libvirt permissions.

Import checks the qcow2 header, rejecting backing chains, encryption, and
incompatible feature bits, then streams a copy into a new libvirt volume.
It checks that the source did not change during the operation. QEMU remains
responsible for full format validation. Source files are never modified.

Each created disk has a UUID-derived name, namespaced domain metadata, and a
durable user-data ownership record scoped by connection URI hash. Disk deletion
requires all identities and the current volume path/key to match. Other VM
definitions are checked for disk/backing-store references, including symlink and
hardlink aliases. Missing unrelated media does not prevent cleanup; inaccessible
or unparseable references do. Unknown ownership means keep the disk. Rollback
refuses a running domain or uncertain state. Both rollback and removal recheck
all domains after undefining and before deleting storage, so a racing start
that leaves a transient guest keeps its disk. Incomplete cleanup keeps ownership
evidence and reports the error.
Cross-client storage changes cannot be made atomic with libvirt; callers should
not concurrently reassign disks while confirming deletion.

Resource edits preserve unrelated XML. Renaming rolls back if redefining
resources fails. CPU topology is normalized to one socket and the chosen core
count. Advanced CPU pinning, hotplug layouts, snapshots, or unusual domain
configuration can be rejected by libvirt rather than silently stripped.

## Automatic Session proxy routing

`pirwa-network.c` reads the manual GNOME proxy settings on the backend worker at
Start or after creating a VM. The persisted `use-host-proxy` preference defaults
to true; it applies only to `qemu:///session`. No configured proxy, a disabled
preference, and other connection URIs retain the ordinary libvirt start path.
PAC settings fail explicitly rather than silently starting with direct access.
Proxy changes are snapshots taken at the next full start, not live updates.

With a proxy selected, the worker starts `libexec/pirwa-network`, a separate C17
gateway using libslirp >= 4.7 and GIO. A private socket pair carries bounded
configuration and readiness messages, including any HTTP authentication details;
credentials are not placed in arguments, environment variables, files or logs.
A one-use loopback TCP listener carries QEMU's length-prefixed Ethernet stream.
The live domain XML substitutes one ordinary userspace, bridge or virtual-network
NIC with a libvirt client interface, retaining its MAC, model and guest device
identity. Plain bridge and network sources are removed only from this live XML;
the gateway's private DHCP network replaces the usual LAN attachment for this
boot. Custom network sources, routes, filters, forwarding, VLANs, virtual ports,
drivers, scripts and multiple NICs are rejected. Start errors appear in a wrapping
dialog; unsupported proxy configurations also offer a Preferences button.
`virDomainCreateXML`
starts the existing inactive UUID with this live XML while retaining the saved
definition. Discovery never changes XML. Managed memory saves require the normal
start path because their saved network cannot be replaced safely.

The gateway supplies DHCP (`10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`).
Internally, restricted libslirp uses a /0 network solely to allow its public
`slirp_add_unix` guest-forward API to terminate arbitrary IPv4 TCP destinations.
The DHCP reply advertises the private /24 instead of this internal match range.
Built-in DNS and host loopback access are disabled; the unused internal DNS
address differs from the guest-visible DNS forward for compatibility with older
libslirp versions. Guest TCP streams terminate at private Unix sockets and are
relayed through GIO SOCKS5 or HTTP CONNECT connections. Libslirp cannot itself
open Internet connections in this restricted configuration.

SOCKS5 is preferred. With HTTP settings, port 443 can select the desktop HTTPS
proxy and other ports select its HTTP proxy. Credentials are shared across HTTP
and HTTPS only when they identify the same proxy endpoint. Explicit IP/CIDR
exclusions retain direct access; hostname exclusions cannot be inferred from IP
packets. A failed proxy never selects a direct fallback. DNS datagrams become
DNS-over-TCP queries through the same proxy; oversized UDP answers request a TCP
retry. The resolver defaults to `1.1.1.1` and can be changed with
`proxy-dns-address`. IPv6, non-DNS UDP, ICMP and fragments are dropped, not sent
through a second network path.

One poll loop handles established relays with fixed 32 KiB buffers per direction.
Eight workers handle blocking connection setup and DNS, with at most 32 pending
jobs, 256 relays, 256 destination listeners, 2,048 tracked TCP states, and a
4 MiB guest output queue. Unix accepts are drained after libslirp opens a forward
to prevent a full Unix backlog from blocking the packet loop. The gateway owns
its sockets and private temporary directory independently of GTK. It survives
application closure, cancels pending work and frees its resources on QEMU
disconnect, and exits if no QEMU client arrives within 30 seconds. Failed or
cancelled VM starts stop their gateway. A gateway failure disconnects networking;
it does not switch to direct access. Restarting the VM creates a new gateway.

The headless tests cover real gateway packets against local SOCKS/HTTP fixtures,
DHCP, TCP payloads and half-close, UDP and TCP DNS, denied-proxy behavior, blocked
protocols, bounded pending connects, cancellation, and malformed framing. The
test-driver start fixture runs the production launcher and verifies saved XML
preservation, ordinary starts, cancellation and failure cleanup. This does not
certify a real QEMU/SELinux deployment or guest application compatibility.

References: [libslirp guest forwarding API](https://gitlab.freedesktop.org/slirp/libslirp/-/blob/master/src/libslirp.h),
[libvirt TCP client interface](https://libvirt.org/formatdomain.html#tcp-tunnel).

## Dependencies and boundaries

Libvirt-glib provides the supported GNOME event adapter; do not run libvirt's
default GLib event loop in a second thread. Libxml2 handles XML escaping and
parsing. Libosinfo identifies media. No database, web runtime, extra daemon,
or embedded GTK 3 widgets are introduced. The native packages use distribution
libraries. The Flatpak bundles client libraries on GNOME Platform 51 and connects
to the host's libvirt sockets, QEMU and storage. It bundles neither QEMU nor a
libvirt daemon. Fixed host-command wrappers provide Boxes Flatpak discovery and
the external virt-viewer fallback. Its host file and D-Bus permissions are
documented in [publishing](../release/publishing.md).

The development build targets local x86_64 Linux guests, DEB/RPM and Flatpak.
Multiple guest monitors, accelerated display scanout, remote management,
snapshots, passthrough and unattended installs remain deferred.
