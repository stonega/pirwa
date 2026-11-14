# Architecture

## Runtime

One native C17 desktop executable uses GTK 4/libadwaita on the main thread.
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
GtkListView recycles rows and a filter model searches VM and OS names. Each row
uses the OS name from libosinfo metadata when known, with its state and VM name
underneath; missing or unknown metadata falls back to the VM name. The backend
reads OS metadata once per discovered UUID and caches missing values too.
Redefining a VM, refreshing manually, or reconnecting invalidates this cache;
removed UUIDs are pruned. The libosinfo database loads lazily on the worker.
Full domain XML and storage details are loaded only for the selected VM.
The search field is hidden until the sidebar's Search button or Ctrl+F opens it;
the button toggles it and Escape closes it. The search field and machine rows use
matching horizontal insets. The sidebar remains resizable without a visible
divider between the list and details.
The search bar uses libadwaita's inline style so its surrounding area blends
with the sidebar in light, dark, and inactive-window states.
Its header and collection use libadwaita's sidebar background and foreground
colors, including the inactive-window variant, in both light and dark themes.
Each pane owns its header, so Add, Search, and the main menu span the sidebar
width, while machine controls sit at the left of the
content header. Host information, live VM usage, and the selected machine's state
share the same row on the right. Start and Shut Down use prominent text buttons;
Shut Down and Restart
are visible only while the machine is running. Header heights stay aligned as
the sidebar is resized.
The connection selector is a custom row inside the main menu and retains the
same connection-switching and Boxes library logic. Preferences and About use
native libadwaita dialogs. Appearance preferences update the shared style
manager for the current application session.
The existing application SVG is embedded as a GResource so native About dialogs
can show it in both development builds and installed copies.
Each row's three-dot menu selects that VM and carries its UUID for Settings,
Remove, and Force Stop. A stale target cannot act on a different selection.

One foreground command runs at a time in the UI. The worker serializes commands
and reads; lifecycle refreshes are coalesced. Selecting another VM cancels old
detail requests. Switching connections uses a new backend, so late callbacks
from the old backend are discarded.

Each backend completion has a monotonically increasing serial. Snapshot and
connection-error delivery use the same ordering fence, so an older callback
cannot replace newer state. Selected-detail requests have a separate generation
counter, including when the same UUID is selected again. Settings fetches fresh
details before opening. Progress carries the foreground request ID; queued
progress from an earlier job cannot update a later job's dialog.

The right pane has no bottom bar. Its single-row header shows controls on the
left, with host architecture, CPU count, total RAM, selected-VM CPU, resident
memory (RSS), network receive/transmit rates, and VM state grouped on the right.
Usage appears while the VM is
running. Long readings ellipsize, with full values in
tooltips. Foreground progress and Cancel appear in a temporary strip below the
header. One read-only bulk libvirt sample runs on the backend worker
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

The source is opened read-only without following symlinks. ISO media must be
bootable and is identified with libosinfo. Guest defaults are generic Linux
virtio defaults; the user explicitly chooses CPU, RAM, and disk capacity.
KVM, host-passthrough CPU, and SPICE must appear in domain capabilities.
UEFI is selected when available unless legacy BIOS is requested. Passt is used
when supported and installed; the userspace default is the fallback.

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

## Dependencies and boundaries

Libvirt-glib provides the supported GNOME event adapter; do not run libvirt's
default GLib event loop in a second thread. Libxml2 handles XML escaping and
parsing. Libosinfo identifies media. No database, web runtime, extra daemon,
embedded GTK 3 widgets, or Flatpak runtime is introduced.

The development build targets local x86_64 Linux guests and native packaging.
Multiple guest monitors, accelerated display scanout, remote management,
snapshots, passthrough, unattended installs,
and Flatpak distribution remain deferred.
