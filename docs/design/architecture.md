# Architecture

## Decision

Build one native desktop executable using C17, GTK 4, libadwaita, and the libvirt
C API. Meson/Ninja matches the GNOME build ecosystem. C gives direct access to
these libraries and avoids another runtime. It is not evidence of a measured
speed advantage; startup, allocations, RPC volume, and display costs must be
measured. Use clang-format rather than introducing Bun/Biome into a C project.

QEMU/KVM runs guests. Libvirt owns domain configuration, lifecycle, storage pools,
and connections. Pirwa manages those resources through libvirt; it must not
launch QEMU directly or edit libvirt's private files.

## Current scaffold

```text
main.c                    AdwApplication, single-instance activation, --version
  └─ pirwa-window.c       GTK list model, window and page states
       └─ pirwa-inventory.c
            GTask worker → libvirt read-only connection → bulk domain state query
            → owned PirwaMachine snapshots → initiating main context
       └─ pirwa-machine.c immutable GObject values: UUID, name, state
```

The window can have one discovery request in flight. It disables refresh until
completion and cancels the request when closed. Cancellation prevents result
delivery to closed UI; it cannot interrupt a synchronous libvirt call already
in progress. A worker closes its connection and frees its domain records on all
paths. Errors are copied before cleanup can replace libvirt's last error.

The initial backend reconnects on each manual refresh. It uses
`virConnectGetAllDomainStats` for state, with locally available name/UUID metadata.
It does not fetch domain XML, open a display, query storage, or take screenshots
for each row. `GtkListView` creates widgets for visible rows. One model splice
publishes a completed inventory; incremental updates belong to M1.

## v0.1 evolution

- **Connection service:** one persistent connection for the selected URI, a
  serialized command queue, event registration, disconnect recovery, and a
  bounded retry policy. A dedicated backend main context can use libvirt-glib's
  event adapter. Register it before opening connections and marshal events to
  GTK. Add libvirt-glib only when this work begins.
- **Model:** identify domains by `(connection URI, UUID)`, never transient domain
  IDs or names. Update existing items from lifecycle events and reconcile one
  bulk snapshot after reconnect. No periodic full-list rebuilds.
- **Commands:** serialize conflicting operations per domain. Recheck live state
  before mutations and expose pending/failed operations in the UI. Do not turn a
  timed-out shutdown into a forced stop automatically.
- **Storage/provisioning:** use libvirt storage pools and volumes. Use escaped XML
  construction with libxml2; add libosinfo when guest defaults are implemented.
  Roll back only resources created by the current operation. Track disk ownership
  explicitly; browsing never rewrites an existing domain.
- **Display:** keep graphics behind a console adapter. Resolve GTK 4 feasibility
  in M0 before selecting domain display defaults. An external supported viewer
  is the v0.1 fallback; embedded display must pass the documented gate.
- **Telemetry:** optional bulk sampling only while resource details are visible.
  Stop sampling when hidden. The collection uses lifecycle events without timers.

## Boundaries

The first release targets a local Linux desktop and one selected QEMU connection.
Session mode is the default. System mode is explicit and uses normal libvirt /
polkit authorization; never run the GUI as root. Remote connections, migration,
background services, and a database are outside the first release.

Native packaging comes first. Flatpak needs a separate evaluation of host libvirt
sockets, storage permissions, console transport, and the security model. Do not
assume Boxes' bundled Flatpak architecture is required for this app.

Primary references: [libvirt API concepts](https://libvirt.org/api.html),
[QEMU connection modes](https://libvirt.org/drvqemu.html), and
[AdwApplication](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.Application.html).
