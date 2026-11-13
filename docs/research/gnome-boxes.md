# GNOME Boxes reference review

Reviewed the default branch of [GNOME/gnome-boxes](https://github.com/GNOME/gnome-boxes)
at commit `78cd19e85b70e796aa17ca26fde611e9bd31608a` during project initialization
on 2026-11-13. The Meson project version at this revision is 50.0. This review
informed architecture; Pirwa does not copy Boxes source code.

## Observed patterns

- [`src/libvirt-broker.vala`](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/libvirt-broker.vala)
  manages connections, asynchronously opens/fetches domains, wraps domains as
  machines, and listens for domain-added/domain-removed signals. Pirwa should
  also keep connection management separate from collection widgets.
- [`src/vm-configurator.vala`](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/vm-configurator.vala)
  builds guest hardware configuration using capability and OS information.
  Pirwa needs capability-aware defaults, not one hard-coded XML for every host.
- [`src/spice-display.vala`](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/spice-display.vala)
  separates display sessions, channels, input, audio, and guest-agent behavior
  from domain lifecycle. Pirwa will use an explicit display adapter boundary.
- [`src/machine.vala`](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/machine.vala)
  has screenshot timers and thumbnail processing. Pirwa's initial collection
  deliberately has no live previews to keep that work out of the idle path.
- [`src/meson.build`](https://github.com/GNOME/gnome-boxes/blob/78cd19e85b70e796aa17ca26fde611e9bd31608a/src/meson.build)
  uses Vala/C, GTK 3, libhandy, libvirt-gobject/gconfig, libosinfo, and GTK 3 SPICE.
  Those display widgets cannot simply be placed in Pirwa's GTK 4 widget tree.

These observations do not demonstrate a performance problem in Boxes. Pirwa
needs its own measurements and a comparable workload before making comparisons.

## Open investigation

Prove a supported GTK 4 display path before committing to embedded consoles.
[libmks](https://gnome.pages.gitlab.gnome.org/libmks/libmks1/) offers GTK 4 QEMU
display integration, but the reviewed API is `1.0.alpha`.
[QEMU D-Bus display](https://www.qemu.org/docs/master/interop/dbus-display.html)
is a possible transport. Verify libvirt setup, FD transport, distro packages,
input, audio, and measured rendering cost in M0. A supported external viewer is
the fallback for v0.1. No display library is added to the scaffold prematurely.
