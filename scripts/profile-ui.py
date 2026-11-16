#!/usr/bin/env python3
"""Explicit native GTK QA profiling. Uses only isolated libvirt test-driver XML."""
import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import tempfile
import time
from collections import Counter
from itertools import pairwise
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


def presentation_summary(sample):
    feedback = sample.get("presentation_feedback")
    if not feedback:
        return None
    if sample.get("feedback_dropped"):
        raise RuntimeError("Truncated compositor feedback cannot support a percentile.")
    expected = ["frame_counter", "before_paint_us", "after_paint_us", "damage_received_us",
                "complete", "presentation_us", "refresh_interval_us", "result"]
    if feedback["columns"] != expected:
        raise RuntimeError("Unrecognized compositor feedback columns.")
    start, end = sample["sample_start_monotonic_us"], sample["sample_end_monotonic_us"]
    rows = [r for r in feedback["samples"] if start <= r[1] < end]
    accepted = [r for r in rows if r[4] and r[7] == "presented" and r[1] > 0 and r[5] >= r[1]]
    latencies = [(r[5] - r[1]) / 1000 for r in accepted]
    times = sorted({r[5] for r in accepted})
    intervals = [(b - a) / 1000 for a, b in pairwise(times)]
    return {
        "scope": "Complete presented GDK feedback during the sampling window; display type/refresh remain separate conditions.",
        "feedback_results": dict(Counter(r[7] for r in rows)),
        "accepted_presentations": len(accepted),
        "before_paint_to_presentation_p95_ms": percentile(latencies, .95) if latencies else None,
        "presentation_interval_p95_ms": percentile(intervals, .95) if intervals else None,
        "duplicate_presentation_timestamps": len(accepted) - len(times),
        "intervals_over_16_7ms": sum(t > 16.7 for t in intervals),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-perf")
    parser.add_argument("--domains", type=int, default=100)
    parser.add_argument("--launches", type=int, default=30)
    parser.add_argument("--idle-seconds", type=int, default=60)
    parser.add_argument("--scroll-seconds", type=int, default=0)
    parser.add_argument("--presentation", action="store_true", help="Collect GTK 4.24+ compositor feedback")
    parser.add_argument("--application", action="store_true", help="Profile the unchanged production executable (isolated desktop, startup only)")
    parser.add_argument("--display-kind", choices=["physical", "virtual", "unspecified"], default="unspecified")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.application:
        if (os.environ.get("PIRWA_QA_ISOLATED") != "1" or args.idle_seconds or args.scroll_seconds):
            parser.error("Production startup mode needs an isolated desktop and --idle-seconds 0 without scrolling.")
        args.presentation = True
    if not (0 <= args.domains <= 1000 and 1 <= args.launches <= 100
            and 0 <= args.idle_seconds <= 300 and 0 <= args.scroll_seconds <= 300):
        parser.error("Use 0–1000 domains, 1–100 launches, and 0–300 sample seconds.")
    if args.scroll_seconds and args.domains < 100:
        parser.error("Use at least 100 domains for a scroll sample.")
    binary = (ROOT / args.build / ("src/pirwa" if args.application else "tests/test-ui")).resolve()
    module = (ROOT / args.build / "tests/qa-application-profile.so").resolve()
    if args.application and not module.is_file():
        parser.error("Build qa-application-profile first.")
    schema = (ROOT / args.build / "data/gschemas.compiled").resolve()
    if not binary.is_file() or not schema.is_file():
        parser.error("Build test-ui and matching GSettings schemas first.")
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
        "graphics_environment": {key: env[key] for key in
                                 ["VK_DRIVER_FILES", "VK_ICD_FILENAMES", "DRI_PRIME",
                                  "MESA_VK_DEVICE_SELECT", "__NV_PRIME_RENDER_OFFLOAD"] if key in env},
        "domains": args.domains,
        "display_kind": args.display_kind,
        "production_executable": args.application,
        "observer_sha256": hashlib.sha256(module.read_bytes()).hexdigest() if args.application else None,
        "presentation_requested": args.presentation,
        "schema_sha256": hashlib.sha256(schema.read_bytes()).hexdigest(),
        "settings_backend": "memory",
        "scope": ("Production executable and unique GApplication lifecycle with an explicit preload observer in an isolated desktop. Does not measure launcher activation or cold caches."
                  if args.application else "Shared production window/backend in a non-unique QA application. Does not measure production bus activation or cold caches.")
                 + " Pinned binary/schema, in-memory settings; after-paint is submission, complete presented feedback is compositor timing.",
        "launch_samples": [],
        "failures": [],
        "completed": False,
    }
    packages = subprocess.run(
        ["rpm", "-q", "glib2", "gtk4", "libadwaita", "libvirt-libs", "libvirt-glib", "libosinfo"],
        capture_output=True, text=True, check=False)
    report["packages"] = packages.stdout.splitlines()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    with tempfile.TemporaryDirectory(prefix="pirwa-profile-") as temp:
        pinned = Path(temp) / "test-ui"
        shutil.copy2(binary, pinned)
        if args.application:
            if env.get("LD_PRELOAD"):
                parser.error("Clear LD_PRELOAD before profiling the production executable.")
            observer = Path(temp) / "qa-application-profile.so"
            shutil.copy2(module, observer)
            env["LD_PRELOAD"] = str(observer)
        capture = Path(temp) / "application-profile.json"
        schemas = Path(temp) / "schemas"
        schemas.mkdir()
        shutil.copy2(schema, schemas / "gschemas.compiled")
        env.update(GSETTINGS_SCHEMA_DIR=str(schemas), GSETTINGS_BACKEND="memory")
        uri = fixture(Path(temp), args.domains)

        def run(*extra):
            started = time.monotonic_ns() // 1000
            if args.application:
                capture.unlink(missing_ok=True)
                env.update(PIRWA_LIBVIRT_URI=uri, PIRWA_PROFILE_OUTPUT=str(capture),
                           PIRWA_PROFILE_DOMAINS=str(args.domains))
                command = [str(pinned)]
            else:
                command = [str(pinned), "--uri", uri]
                if args.presentation:
                    command.append("--presentation")
                command.extend(extra)
            try:
                result = subprocess.run(command, env=env, check=False,
                                        capture_output=True, text=True, timeout=340)
            except subprocess.TimeoutExpired as error:
                def decoded(value):
                    return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
                report["failures"].append({"command": command, "timeout_seconds": 340,
                                           "stdout": decoded(error.stdout), "stderr": decoded(error.stderr)})
                save()
                raise RuntimeError("UI profiling process exceeded its watchdog.") from error
            if result.returncode:
                report["failures"].append({"command": command, "returncode": result.returncode,
                                           "stdout": result.stdout, "stderr": result.stderr})
                save()
                raise RuntimeError(f"UI profiling failed ({result.returncode}): {result.stderr}\n{result.stdout}")
            if args.application:
                if not capture.is_file():
                    raise RuntimeError("The production startup observer did not return a capture.")
                sample = json.loads(capture.read_text())
                if not sample["profile_complete"]:
                    report["failures"].append({"command": command, "capture": sample, "stderr": result.stderr})
                    save()
                    raise RuntimeError("The production window did not receive complete presented feedback.")
            else:
                sample = json.loads(result.stdout)
            if not args.application and sample["domains"] != args.domains:
                raise RuntimeError(f"Expected {args.domains} domains; got {sample['domains']}")
            sample["launch_to_after_paint_ms"] = (
                sample["first_paint_monotonic_us"] - started) / 1000
            sample["launch_to_ready_ms"] = (sample["ready_monotonic_us"] - started) / 1000 if sample["ready_monotonic_us"] else None
            if args.presentation:
                presented = sample.get("first_presentation_monotonic_us", 0)
                if presented <= 0:
                    raise RuntimeError("The QA binary did not return a presented first frame.")
                sample["launch_to_first_presentation_ms"] = (presented - started) / 1000
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
        if args.presentation:
            values = [sample["launch_to_first_presentation_ms"] for sample in report["launch_samples"]]
            report["launch_presentation_summary_ms"] = {
                "p50": statistics.median(values), "p95": percentile(values, .95), "samples": len(values),
            }
        if args.idle_seconds:
            print(f"idle sample: {args.idle_seconds}s", flush=True)
            report["idle"] = run("--duration", str(args.idle_seconds))
            save()
        if args.scroll_seconds:
            print(f"scroll sample: {args.scroll_seconds}s", flush=True)
            report["scroll"] = run("--duration", str(args.scroll_seconds), "--scroll")
            save()
            costs = report["scroll"]["frame_work_us"]
            if not costs:
                raise RuntimeError("The scroll sample did not produce frame work.")
            report["scroll"]["paint_wall_p95_ms"] = percentile(costs, 0.95) / 1000
            cpus = report["scroll"].get("frame_cpu_us", [])
            report["scroll"]["paint_thread_cpu_p95_ms"] = percentile(cpus, .95) / 1000 if cpus else None
            if args.presentation:
                report["scroll"]["compositor"] = presentation_summary(report["scroll"])
        report["host_load_at_end"] = os.getloadavg()
        report["completed"] = True
        save()
    print(json.dumps({
        "launch": report["launch_summary_ms"],
        "presentation": report.get("launch_presentation_summary_ms"),
        "idle": {key: report.get("idle", {}).get(key) for key in ("sample_cpu_percent", "pss_kib", "sample_frames")},
        "scroll_p95_ms": report.get("scroll", {}).get("paint_wall_p95_ms"),
        "output": str(args.output),
    }))


if __name__ == "__main__":
    main()
