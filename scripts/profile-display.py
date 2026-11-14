#!/usr/bin/env python3
"""Profile the production GTK console renderer with synthetic framebuffers.

Requires a graphical display. Never connects to libvirt or changes a VM.
"""

import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-perf")
    parser.add_argument("--frames", type=int, default=180)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument(
        "--baseline-binary",
        type=Path,
        help="Compare against a saved benchmark binary, alternating run order",
    )
    parser.add_argument("--sizes", default="1280x720,1920x1080,3840x2160")
    parser.add_argument("--idle-seconds", type=int, default=10)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        sizes = [tuple(map(int, size.split("x"))) for size in args.sizes.split(",")]
        if not 1 <= len(sizes) <= 5 or len(set(sizes)) != len(sizes):
            raise ValueError()
        for width, height in sizes:
            if not (64 <= width <= 8192 and 64 <= height <= 8192):
                raise ValueError()
    except ValueError:
        parser.error("Use 1–5 unique WxH sizes with dimensions from 64 to 8192.")
    if not (
        30 <= args.frames <= 1800
        and 1 <= args.runs <= 10
        and 0 <= args.idle_seconds <= 60
    ):
        parser.error("Use 30–1800 frames, 1–10 runs, and 0–60 idle seconds.")
    binary = (ROOT / args.build / "tests/benchmark-display").resolve()
    binaries = {"current": binary}
    if args.baseline_binary:
        binaries["baseline"] = args.baseline_binary.resolve()
    source_files = [
        "src/pirwa-display.c",
        "src/pirwa-display.h",
        "src/pirwa-frame.c",
        "src/pirwa-frame.h",
        "tests/benchmark-display.c",
    ]
    power = Path("/sys/firmware/acpi/platform_profile")
    packages = subprocess.run(
        [
            "rpm",
            "-q",
            "gtk4",
            "glib2",
            "spice-glib",
            "gnome-shell",
            "mutter",
            "mesa-dri-drivers",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    report = {
        "scope": "Shared production GTK display renderer; synthetic xRGB framebuffer. "
        "Frame-clock spans include the Update phase and end at after-paint; they exclude compositor presentation/GPU completion. "
        "Total process CPU includes synthetic pixel generation; its main-thread CPU is also "
        "recorded per update. No SPICE decoding, guest workload or libvirt is measured.",
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_dirty": bool(
            subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=ROOT, text=True
            )
        ),
        "source_sha256": {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in source_files
        },
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "compared_binaries": {
            name: {
                "path": str(path),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            for name, path in binaries.items()
        },
        "qa_runtime_directory": os.environ.get("XDG_RUNTIME_DIR")
        if os.environ.get("PIRWA_QA_ISOLATED") == "1"
        else None,
        "platform": platform.platform(),
        "distribution": platform.freedesktop_os_release().get("PRETTY_NAME"),
        "cpu_model": next(
            (
                line.split(":", 1)[1].strip()
                for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")
            ),
            None,
        ),
        "allowed_cpus": sorted(os.sched_getaffinity(0)),
        "power_profile": power.read_text().strip() if power.exists() else None,
        "requested_renderer": os.environ.get("GSK_RENDERER", "default"),
        "requested_backend": os.environ.get("GDK_BACKEND", "default"),
        "requested_a11y": os.environ.get("GTK_A11Y", "default"),
        "packages": packages.stdout.splitlines(),
        "host_load_at_start": os.getloadavg(),
        "frames_per_run": args.frames,
        "runs": [],
        "summary": [],
    }
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    snapshot_dir = args.output.parent / (args.output.stem + "-snapshots")
    snapshot_dir.mkdir(exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    modes = ["full", "small", "burst"]
    for repeat in range(args.runs):
        # Rotate order to reduce the influence of warm caches and host drift.
        cases = [(width, height, mode) for width, height in sizes for mode in modes]
        shift = repeat % len(cases)
        for width, height, mode in cases[shift:] + cases[:shift]:
            variants = list(binaries)
            if repeat % 2:
                variants.reverse()
            for variant in variants:
                command = [
                    str(binaries[variant]),
                    "--width",
                    str(width),
                    "--height",
                    str(height),
                    "--frames",
                    str(args.frames),
                    "--mode",
                    mode,
                ]
                if repeat == 0:
                    command += [
                        "--snapshot",
                        str(snapshot_dir / f"{variant}-{width}x{height}-{mode}.png"),
                    ]
                if repeat == 0 and (width, height) == sizes[0] and mode == "small":
                    command += ["--idle-seconds", str(args.idle_seconds)]
                result = subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=240 + args.idle_seconds,
                    check=False,
                )
                if result.returncode:
                    save()
                    raise RuntimeError(
                        f"Renderer QA failed ({result.returncode}): {result.stderr}\n{result.stdout}"
                    )
                sample = json.loads(result.stdout)
                if (
                    len(sample["samples"]) != args.frames
                    or not sample["correctness_passed"]
                ):
                    raise RuntimeError("Renderer QA produced an incomplete sample")
                sample["repeat"] = repeat
                sample["variant"] = variant
                sample["command"] = command
                if result.stderr.strip():
                    sample["diagnostics"] = result.stderr.strip()
                report["runs"].append(sample)
                save()
                print(
                    f"{repeat + 1}/{args.runs} {variant} {width}x{height} {mode}: "
                    f"{sample['cpu_percent_one_core']:.2f}% CPU, "
                    f"paint p95 {percentile([s['paint_wall_us'] for s in sample['samples']], 0.95) / 1000:.3f} ms",
                    flush=True,
                )
    for width, height in sizes:
        for mode in modes:
            for variant in binaries:
                runs = [
                    r
                    for r in report["runs"]
                    if (r["width"], r["height"], r["mode"], r["variant"])
                    == (width, height, mode, variant)
                ]
                frames = [s for r in runs for s in r["samples"]]
                summary = {
                    "width": width,
                    "height": height,
                    "mode": mode,
                    "variant": variant,
                    "runs": len(runs),
                    "frames": len(frames),
                    "median_cpu_percent_one_core": statistics.median(
                        r["cpu_percent_one_core"] for r in runs
                    ),
                    "median_pss_end_mib": statistics.median(
                        r["pss_end_kib"] / 1024 for r in runs
                    ),
                    "median_update_hz": statistics.median(
                        len(r["samples"]) * 1e6 / r["elapsed_us"] for r in runs
                    ),
                }
                for field in [
                    "paint_wall_us",
                    "paint_cpu_us",
                    "producer_cpu_us",
                    "interval_us",
                ]:
                    summary[field + "_p50"] = statistics.median(
                        s[field] for s in frames
                    )
                    summary[field + "_p95"] = percentile(
                        [s[field] for s in frames], 0.95
                    )
                report["summary"].append(summary)
    report["host_load_at_end"] = os.getloadavg()
    save()
    print(f"Saved {args.output}", flush=True)


if __name__ == "__main__":
    main()
