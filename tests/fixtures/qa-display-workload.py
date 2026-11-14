#!/usr/bin/env python3
"""Controlled GTK drawing INSIDE an explicitly disposable graphical guest."""

import argparse
import json
import os
import signal
import time
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["idle", "small", "full"], required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    if os.environ.get("PIRWA_RUN_KVM_TESTS") != "1" or not 5 <= args.seconds <= 3600:
        parser.error("Use only in a disposable guest with PIRWA_RUN_KVM_TESTS=1.")
    if args.status.parent != Path("/tmp") or not args.status.name.startswith(
        "pirwa-display-"
    ):
        parser.error("Use /tmp/pirwa-display-NAME.json for status.")

    started = time.monotonic()
    state = {
        "mode": args.mode,
        "ticks": 0,
        "draws": 0,
        "width": 0,
        "height": 0,
        "complete": False,
    }
    window = Gtk.Window(title="Pirwa disposable display workload")
    canvas = Gtk.DrawingArea()
    window.add(canvas)
    window.connect("destroy", Gtk.main_quit)

    def draw(widget, cr):
        width, height = widget.get_allocated_width(), widget.get_allocated_height()
        state.update(
            width=width,
            height=height,
            scale=widget.get_scale_factor(),
            draws=state["draws"] + 1,
        )
        cr.set_source_rgb(0.12, 0.16, 0.20)
        cr.paint()
        phase = (state["ticks"] % 60) / 60
        if args.mode == "full":
            for y in range(0, height, 32):
                cr.set_source_rgb(phase, (y / max(1, height) + phase) % 1, 1 - phase)
                cr.rectangle(0, y, width, min(32, height - y))
                cr.fill()
        else:
            cr.set_source_rgb(phase, 1 - phase, 0.5)
            cr.rectangle(width // 2 - 32, height // 2 - 32, 64, 64)
            cr.fill()

    def tick():
        state["ticks"] += 1
        if args.mode == "small":
            canvas.queue_draw_area(
                canvas.get_allocated_width() // 2 - 32,
                canvas.get_allocated_height() // 2 - 32,
                64,
                64,
            )
        else:
            canvas.queue_draw()
        return GLib.SOURCE_CONTINUE

    def status():
        state.update(elapsed_seconds=time.monotonic() - started)
        temporary = args.status.with_suffix(".tmp")
        temporary.write_text(json.dumps(state) + "\n")
        temporary.replace(args.status)
        return GLib.SOURCE_CONTINUE

    def stop():
        state["complete"] = True
        status()
        window.destroy()
        return GLib.SOURCE_REMOVE

    canvas.connect("draw", draw)
    window.fullscreen()
    window.show_all()
    if args.mode != "idle":
        GLib.timeout_add(33, tick)  # Requested cadence; actual draws are recorded.
    GLib.timeout_add(1000, status)
    GLib.timeout_add_seconds(args.seconds, stop)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, stop)
    Gtk.main()


if __name__ == "__main__":
    main()
