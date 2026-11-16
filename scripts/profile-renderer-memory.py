#!/usr/bin/env python3
"""Freeze and profile renderer memory without connecting to libvirt.

Prepare outside the desktop with --prepare-only, then run the same output
directory inside test-gnome.py. Nothing is compiled while measurements run.
"""

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=True, **kwargs)


def prepare(output):
    output.mkdir(mode=0o700, parents=True)
    source = output / "source"
    source.mkdir()
    inputs = [
        "src/pirwa-display.c",
        "src/pirwa-display.h",
        "src/pirwa-frame.c",
        "src/pirwa-frame.h",
        "tests/benchmark-display.c",
        "tests/benchmark-display-memory.c",
        "tests/qa-frame-feedback.c",
        "tests/qa-frame-feedback.h",
    ]
    hashes = {}
    for name in inputs:
        data = (ROOT / name).read_bytes()
        (source / Path(name).name).write_bytes(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
    # Refuse a snapshot spanning simultaneous edits to the relevant inputs.
    if any(sha(ROOT / name) != digest for name, digest in hashes.items()):
        raise RuntimeError(
            "Renderer sources changed while freezing; use a fresh output"
        )
    shutil.copy2(ROOT / "build-perf/config.h", source / "config.h")
    original = (source / "pirwa-display.c").read_text()
    start = original.index("  /* A luminance mask composites")
    direct = original.find("  /* A single opaque texture filling")
    if direct >= 0:
        start = direct
    end = original.index(
        "  gtk_snapshot_append_color (snapshot, &black, &bounds);", start
    )
    flat = original[:start] + original[end:]
    closing = "  gtk_snapshot_pop (snapshot);\n  gtk_snapshot_restore (snapshot);\n}\n\nstatic void\ndispose"
    if flat.count(closing) != 1:
        raise RuntimeError("Cannot derive a flat control from this renderer")
    flat = flat.replace(
        closing, closing.replace("  gtk_snapshot_pop (snapshot);\n", "")
    )
    (source / "display-flat.c").write_text(flat)
    (source / "frame-rows.c").write_text(
        run(["git", "show", "f332a02:src/pirwa-frame.c"], cwd=ROOT).stdout
    )
    flags = ["-O3", "-g", "-std=c17", "-I" + str(source)]
    deps = shlex.split(
        run(
            ["pkg-config", "--cflags", "--libs", "gtk4", "spice-client-glib-2.0"]
        ).stdout
    )
    variants = {}
    for name, display, frame in [
        ("flat-rows", "display-flat.c", "frame-rows.c"),
        ("flat-packed", "display-flat.c", "pirwa-frame.c"),
        ("rounded-packed", "pirwa-display.c", "pirwa-frame.c"),
    ]:
        variants[name] = {}
        for kind, fixture in [
            ("memory", "benchmark-display-memory.c"),
            ("timing", "benchmark-display.c"),
        ]:
            binary = output / f"{name}-{kind}"
            command = [
                "cc",
                *flags,
                str(source / fixture),
                str(source / display),
                str(source / frame),
                "-o",
                str(binary),
                *deps,
            ]
            if kind == "memory":
                command += [
                    "-Wl,--wrap=pirwa_frame_texture",
                    "-Wl,--wrap=pirwa_frame_texture_rect",
                ]
            else:
                command.append(str(source / "qa-frame-feedback.c"))
            run(command, cwd=ROOT)
            variants[name][kind] = {"path": str(binary), "sha256": sha(binary)}
    manifest = {
        "scope": "Synthetic production renderer. Memory probe observes app-created texture lifetimes, /proc PSS, glibc allocation and GPU fdinfo. No SPICE decoding, VM or physical presentation.",
        "control": "Flat control removes only the outer rounded luminance mask; current transparent empty-state behavior is retained.",
        "source_commit": run(["git", "rev-parse", "HEAD"], cwd=ROOT).stdout.strip(),
        "source_dirty": bool(run(["git", "status", "--porcelain"], cwd=ROOT).stdout),
        "input_sha256": hashes,
        "frozen_sha256": {p.name: sha(p) for p in source.iterdir()},
        "flags": flags,
        "dependency_flags": deps,
        "variants": variants,
        "host": platform.platform(),
        "compiler": run(["cc", "--version"]).stdout.splitlines()[0],
        "packages": run(
            ["rpm", "-q", "glibc", "gtk4", "spice-glib", "gnome-shell", "mutter"]
        ).stdout.splitlines(),
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared frozen controls in {output}", flush=True)


def allocator_records(directory):
    records = []
    for path in sorted(directory.glob("*.xml")):
        tree = ET.fromstring(path.read_text())
        record = {
            "file": path.name,
            "sha256": sha(path),
            "arenas": len(tree.findall("heap")),
        }
        for tag, kind, key in [
            ("system", "current", "system_current_bytes"),
            ("total", "rest", "free_rest_bytes"),
            ("total", "fast", "free_fast_bytes"),
            ("total", "mmap", "mmap_bytes"),
        ]:
            element = tree.find(f"{tag}[@type='{kind}']")
            record[key] = int(element.attrib["size"]) if element is not None else None
        record["drm_fdinfo"] = path.with_suffix(".drm").read_text()
        records.append(record)
    return records


def measure(args):
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/"))
    if os.environ.get("PIRWA_QA_ISOLATED") != "1" or not runtime.parent.name.startswith(
        "pirwa-gnome-"
    ):
        raise RuntimeError("Run measurements inside test-gnome.py's private desktop")
    manifest = json.loads((args.output / "manifest.json").read_text())
    for variants in manifest["variants"].values():
        for item in variants.values():
            if sha(Path(item["path"])) != item["sha256"]:
                raise RuntimeError("A frozen binary changed")
    result_path = args.output / "results.json"
    if result_path.exists():
        raise RuntimeError("Use a newly prepared output directory")
    report = dict(manifest, runs=[], timing_runs=[], failures=[])
    report["observer_scope"] = (
        "Memory sampling/weak refs are diagnostic only. Timing runs use the separate uninstrumented correctness fixture. malloc_trim is called once after the last viewer closes, only as an allocator diagnostic."
    )
    report["requested_renderer"] = os.environ.get("GSK_RENDERER", "default")
    profile = Path("/sys/firmware/acpi/platform_profile")
    report["power_profile"] = profile.read_text().strip() if profile.exists() else "unknown"
    report["started_unix"] = time.time()
    report["measurement_runner_sha256"] = sha(Path(__file__))

    def save():
        result_path.write_text(json.dumps(report, indent=2) + "\n")

    def execute(name, command):
        env = dict(os.environ, GTK_A11Y="test")
        pressure = {
            n: Path("/proc/pressure/" + n).read_text().splitlines()
            for n in ("cpu", "memory", "io")
        }
        completed = subprocess.run(
            command, env=env, text=True, capture_output=True, timeout=400, check=False
        )
        (args.output / (name + ".stderr.log")).write_text(completed.stderr)
        if completed.returncode:
            report["failures"].append(
                {
                    "name": name,
                    "returncode": completed.returncode,
                    "stderr": completed.stderr,
                    "stdout": completed.stdout,
                }
            )
            save()
            raise RuntimeError(f"{name} failed; inspect its diagnostic log")
        result = json.loads(completed.stdout)
        if (result["view_width"], result["view_height"], result["scale"]) != (
            args.width,
            args.height,
            1,
        ):
            raise RuntimeError(f"Unexpected viewport/scale: {name}")
        result.update(command=command, host_pressure_before=pressure)
        return result

    for repeat in range(args.runs):
        order = list(manifest["variants"])
        if repeat % 2:
            order.reverse()
        for variant in order:
            name = f"memory-{repeat}-{variant}"
            heap = args.output / (name + "-heap")
            command = [
                manifest["variants"][variant]["memory"]["path"],
                "--width",
                str(args.width),
                "--height",
                str(args.height),
                "--cycles",
                str(args.cycles),
                "--heap-directory",
                str(heap),
            ]
            result = execute(name, command)
            result.update(
                variant=variant, repeat=repeat, allocator=allocator_records(heap)
            )
            report["runs"].append(result)
            save()
            last = result["samples"][-1]
            print(
                f"Memory {repeat + 1}/{args.runs} {variant}: final PSS {last['pss_kib'] / 1024:.1f} MiB, live texture bytes {last['live_texture_bytes']}",
                flush=True,
            )
    # No allocator or lifetime observers in these timing/correctness processes.
    for repeat in range(args.runs):
        for mode in ("full", "small"):
            order = ["flat-packed", "rounded-packed"]
            if repeat % 2:
                order.reverse()
            for variant in order:
                name = f"timing-{repeat}-{mode}-{variant}"
                command = [
                    manifest["variants"][variant]["timing"]["path"],
                    "--width",
                    str(args.width),
                    "--height",
                    str(args.height),
                    "--frames",
                    "60",
                    "--mode",
                    mode,
                    "--fullscreen",
                ]
                result = execute(name, command)
                if not result["correctness_passed"]:
                    raise RuntimeError("Image correctness check did not pass")
                result.update(variant=variant, repeat=repeat)
                report["timing_runs"].append(result)
                save()
                print(
                    f"Timing {repeat + 1}/{args.runs} {mode} {variant}: {result['cpu_percent_one_core']:.2f}% CPU",
                    flush=True,
                )
    report["completed"] = True
    report["source_changes_observed_after_run"] = {
        name: sha(ROOT / name)
        for name, value in manifest["input_sha256"].items()
        if sha(ROOT / name) != value
    }
    save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--width", type=int, default=3840)
    parser.add_argument("--height", type=int, default=2160)
    args = parser.parse_args()
    if not (
        1 <= args.runs <= 3
        and 1 <= args.cycles <= 5
        and 64 <= args.width <= 3840
        and 64 <= args.height <= 2160
    ):
        parser.error("Use 1–3 runs, 1–5 cycles, and dimensions within 64–3840×2160")
    args.output = args.output.resolve()
    if args.prepare_only:
        prepare(args.output)
    else:
        measure(args)


if __name__ == "__main__":
    main()
