#!/usr/bin/env python3
"""Set scale on the isolated GNOME QA monitor, then run a command.

Refuses physical displays and normal desktop sessions. Configuration is temporary.
"""

import argparse
import os
from pathlib import Path

from gi.repository import Gio, GLib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=float, choices=[1.0, 2.0], default=2.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/"))
    if (
        os.environ.get("PIRWA_QA_ISOLATED") != "1"
        or not runtime.parent.name.startswith("pirwa-gnome-")
        or not args.command
    ):
        parser.error("Run a command only inside test-gnome.py’s private desktop.")
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(method, params=None):
        return bus.call_sync(
            "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            method,
            params,
            None,
            Gio.DBusCallFlags.NONE,
            20000,
            None,
        )

    serial, monitors, logical, _ = call("GetCurrentState").unpack()
    if (
        len(monitors) != 1
        or len(logical) != 1
        or monitors[0][0][:3] != ("Meta-0", "MetaVendor", "MetaVirtualMonitor")
    ):
        parser.error("Expected exactly one Meta-0 virtual QA monitor.")
    current = next(mode for mode in monitors[0][1] if mode[-1].get("is-current"))
    if args.scale not in current[5]:
        parser.error(
            "Requested scale is unsupported; use --monitor 2560x1600@60 on the wrapper."
        )
    configuration = [(0, 0, args.scale, 0, True, [("Meta-0", current[0], {})])]
    call(
        "ApplyMonitorsConfig",
        GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (serial, 1, configuration, {})),
    )
    _, _, applied, _ = call("GetCurrentState").unpack()
    if applied[0][2] != args.scale:
        raise RuntimeError("The compositor did not apply the requested scale.")
    env = os.environ.copy()
    env.pop("GDK_SCALE", None)
    os.execvpe(args.command[0], args.command, env)


if __name__ == "__main__":
    main()
