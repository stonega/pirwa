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
application logic. The console launches virt-viewer with an argument array,
connection URI, and domain UUID.

## Model and work queue

Workers produce owned snapshots. The main thread reconciles them against
canonical GObjects identified by connection and UUID. State changes update
existing objects; list insertions/removals replace only the differing range.
GtkListView recycles rows and a filter model searches names. Domain XML and
storage details are loaded only for the selected VM.

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

There are no collection screenshot or stats timers. A disconnected backend
tries reconnect at 1/2/4/8/16-second intervals, then waits for manual reconnect.
Mutations are never replayed automatically. Healthy connections have no polling
or retry timer.

## GNOME Boxes onboarding

`pirwa-boxes.c` performs one cancellable discovery job away from GTK. It checks
for the native executable or desktop entry, lists persistent Session domains
through a temporary read-only libvirt connection, and ignores guestfs helpers.
This matches Boxes' shared Session model, including machines originally created
by other managers. No domain XML is rewritten and no disk is read or copied.

Stable Boxes Flatpak definitions are read from
`~/.var/app/org.gnome.Boxes/config/libvirt/qemu`. Discovery reads only bounded
regular XML files without following final symlinks; DTD-bearing definitions are
rejected. Machines are deduplicated by UUID within each library. A matching
user-owned runtime libvirt socket supplies the explicit local connection URI.
An offline library offers Open Boxes and Refresh; it is not silently migrated
into the host session. Native and Flatpak libraries remain separate connections.

`pirwa-boxes-dialog.c` presents virtualized machine rows and explains reuse and
ownership. The automatic guide waits until the window is mapped and foreground
work/dialogs have finished. A header action requests a fresh scan manually.
Closing or skipping the guide records completion; a later automatic startup
reads only the small preference file rather than rescanning the libraries.
Choosing Flatpak also remembers the connection. Preferences are written
atomically on a worker under `XDG_STATE_HOME/pirwa/boxes-import.ini`, with a
GApplication hold until the write completes. Import adds no disk ownership.

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
Embedded display, remote management, snapshots, passthrough, unattended installs,
and Flatpak distribution remain deferred.
