#!/usr/bin/env python3
"""Explicit native GTK QA profiling. Uses only isolated libvirt test-driver XML."""
import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


def fixture(directory, count):
    node = ET.Element("node")
    cpu = ET.SubElement(node, "cpu")
    for name, text in (("model", "x86_64"), ("mhz", "2400"), ("active", "4"),
                       ("nodes", "1"), ("sockets", "1"), ("cores", "4"), ("threads", "1")):
        ET.SubElement(cpu, name).text = text
    ET.SubElement(node, "memory").text = "8388608"
    for index in range(count):
        domain = ET.Element("domain", type="test")
        ET.SubElement(domain, "name").text = f"Machine {index:04d}"
        ET.SubElement(domain, "memory", unit="KiB").text = "1048576"
        ET.SubElement(domain, "vcpu").text = "2"
        ET.SubElement(ET.SubElement(domain, "os"), "type").text = "hvm"
        ET.SubElement(domain, "devices")
        name = f"domain-{index:04d}.xml"
        ET.ElementTree(domain).write(directory / name, encoding="utf-8")
        ET.SubElement(node, "domain", file=name)
    path = directory / "node.xml"
    ET.ElementTree(node).write(path, encoding="utf-8")
    return f"test://{path}"


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-perf")
    parser.add_argument("--domains", type=int, default=100)
    parser.add_argument("--launches", type=int, default=30)
    parser.add_argument("--idle-seconds", type=int, default=60)
    parser.add_argument("--scroll-seconds", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not (0 <= args.domains <= 1000 and 1 <= args.launches <= 100
            and 0 <= args.idle_seconds <= 300 and 0 <= args.scroll_seconds <= 300):
        parser.error("Use 0–1000 domains, 1–100 launches, and 0–300 sample seconds.")
    if args.scroll_seconds and args.domains < 100:
        parser.error("Use at least 100 domains for a scroll sample.")
    binary = (ROOT / args.build / "tests/test-ui").resolve()
    env = os.environ.copy()
    # Do not change the display, power profile, caches, renderer, or desktop settings.
    profile_path = Path("/sys/firmware/acpi/platform_profile")
    version = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                             capture_output=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, text=True,
                                capture_output=True, check=True).stdout)
    report = {
        "source_commit": version,
        "qa_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "host_load_at_start": os.getloadavg(),
        "distribution": platform.freedesktop_os_release().get("PRETTY_NAME"),
        "cpu_model": next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")), None),
        "source_dirty": dirty,
        "platform": platform.platform(),
        "power_profile": profile_path.read_text().strip() if profile_path.exists() else None,
        "requested_backend": env.get("GDK_BACKEND", "default"),
        "requested_a11y": env.get("GTK_A11Y", "default"),
        "requested_renderer": env.get("GSK_RENDERER", "default"),
        "domains": args.domains,
        "scope": "Shared production window/backend in a non-unique QA application; after-paint is submission, not presentation. No cold-cache claim.",
        "launch_samples": [],
    }
    packages = subprocess.run(
        ["rpm", "-q", "glib2", "gtk4", "libadwaita", "libvirt-libs", "libvirt-glib", "libosinfo"],
        capture_output=True, text=True)
    report["packages"] = packages.stdout.splitlines()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    with tempfile.TemporaryDirectory(prefix="pirwa-profile-") as temp:
        uri = fixture(Path(temp), args.domains)

        def run(*extra):
            started = time.monotonic_ns() // 1000
            result = subprocess.run([str(binary), "--uri", uri, *extra], env=env,
                                    capture_output=True, text=True, timeout=340)
            if result.returncode:
                raise RuntimeError(f"UI profiling failed ({result.returncode}): {result.stderr}\n{result.stdout}")
            sample = json.loads(result.stdout)
            if sample["domains"] != args.domains:
                raise RuntimeError(f"Expected {args.domains} domains; got {sample['domains']}")
            sample["launch_to_after_paint_ms"] = (
                sample["first_paint_monotonic_us"] - started) / 1000
            sample["launch_to_ready_ms"] = (sample["ready_monotonic_us"] - started) / 1000
            # Preserve diagnostics, including driver/environment warnings.
            if result.stderr.strip():
                sample["diagnostics"] = result.stderr.strip()
            return sample

        report["warmup"] = run()
        save()
        for index in range(args.launches):
            report["launch_samples"].append(run())
            save()
            print(f"launch {index + 1}/{args.launches}", flush=True)
        values = [sample["launch_to_after_paint_ms"] for sample in report["launch_samples"]]
        report["launch_summary_ms"] = {
            "p50": statistics.median(values), "p95": percentile(values, 0.95),
            "samples": len(values),
        }
        if args.idle_seconds:
            print(f"idle sample: {args.idle_seconds}s", flush=True)
            report["idle"] = run("--duration", str(args.idle_seconds))
            save()
        if args.scroll_seconds:
            print(f"scroll sample: {args.scroll_seconds}s", flush=True)
            report["scroll"] = run("--duration", str(args.scroll_seconds), "--scroll")
            costs = report["scroll"]["frame_work_us"]
            if not costs:
                raise RuntimeError("The scroll sample did not produce frame work.")
            report["scroll"]["cpu_frame_work_p95_ms"] = percentile(costs, 0.95) / 1000
        report["host_load_at_end"] = os.getloadavg()
        save()
    print(json.dumps({
        "launch": report["launch_summary_ms"],
        "idle": {key: report.get("idle", {}).get(key) for key in ("sample_cpu_percent", "pss_kib", "sample_frames")},
        "scroll_p95_ms": report.get("scroll", {}).get("cpu_frame_work_p95_ms"),
        "output": str(args.output),
    }))


if __name__ == "__main__":
    main()
