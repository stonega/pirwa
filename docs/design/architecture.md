# Architecture

## Runtime

One native C17 desktop executable uses GTK 4/libadwaita on the main thread.
A persistent backend owns one selected libvirt connection and one worker queue.
All connection, domain, storage, media, and filesystem operations run on that
worker. Libvirt-glib registers event sources with the GTK/GLib main loop before
the connection opens. Event callbacks only enqueue coalesced reconciliation;
they do not make blocking libvirt calls.

    main.c → pirwa-window.c → pirwa-backend.c
                                  ├─ pirwa-inventory.c: one bulk state query
                                  ├─ pirwa-config.c: escaped XML and image checks
                                  └─ pirwa-provision.c: storage, media, ownership

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

There are no collection screenshot or stats timers. A disconnected backend
tries reconnect at 1/2/4/8/16-second intervals, then waits for manual reconnect.
Mutations are never replayed automatically. Healthy connections have no polling
or retry timer.

## Cancellation and shutdown

Cancellation is checked before operations and between image upload chunks.
It cannot interrupt an in-flight synchronous libvirt RPC. After a mutation
commits, the result reports success rather than a misleading cancellation.
Closing the window cancels work, stops the queue, and prevents late UI access.
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
definitions are checked for disk/backing-store references. Unknown ownership
means keep the disk. Cleanup preserves storage if domain rollback fails.
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
