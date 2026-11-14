#!/usr/bin/env python3
"""Profile a real disposable Fedora live guest inside test-gnome.py.

Creates one uniquely named VM/pool, boots the supplied ISO, measures the native
console with controlled guest drawing, then removes only its own resources.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import socket
import statistics
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parent.parent
APP = "io.github.pirwa.ConsoleProfile"
GUEST_LAUNCH = r"""
import glob, os, pwd, subprocess, sys
runtime = f'/run/user/{pwd.getpwnam("liveuser").pw_uid}'
display = next(p for p in glob.glob(runtime+'/wayland-*') if not p.endswith('.lock'))
command = ['runuser', '-u', 'liveuser', '--', 'env', 'PIRWA_RUN_KVM_TESTS=1',
           'GDK_BACKEND=wayland', 'XDG_RUNTIME_DIR='+runtime,
           'WAYLAND_DISPLAY='+os.path.basename(display),
           'DBUS_SESSION_BUS_ADDRESS=unix:path='+runtime+'/bus',
           'python3', '/run/pirwa-qa/qa-display-workload.py',
           '--mode', sys.argv[1], '--status', sys.argv[2], '--seconds', '1200']
with open('/tmp/pirwa-display-workload.log', 'w') as log:
    print(subprocess.Popen(command, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=log, start_new_session=True).pid)
"""


def run(command, *, check=True, timeout=120):
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=timeout, check=False
    )
    if check and result.returncode:
        raise RuntimeError(f"{command[0]} failed: {result.stderr}")
    return result


def guest(channel, argv, timeout=30):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(timeout)
        peer.connect(str(channel))
        peer.sendall((json.dumps(argv) + "\n").encode())
        result = json.loads(peer.makefile().readline())
    if result["code"]:
        raise RuntimeError(result["err"])
    return result["out"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uri", required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", default="build-perf")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument(
        "--boot-seconds", type=int, default=180, help="Guest-agent readiness deadline"
    )
    parser.add_argument(
        "--expect-size", required=True, help="Actual guest framebuffer WxH"
    )
    args = parser.parse_args()
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/"))
    if (
        os.environ.get("PIRWA_RUN_KVM_TESTS") != "1"
        or os.environ.get("PIRWA_QA_ISOLATED") != "1"
        or not runtime.parent.name.startswith("pirwa-gnome-")
    ):
        parser.error(
            "Use PIRWA_RUN_KVM_TESTS=1 inside test-gnome.py’s private desktop."
        )
    if not (
        1 <= args.runs <= 5
        and 5 <= args.seconds <= 30
        and 30 <= args.boot_seconds <= 180
    ):
        parser.error("Use 1–5 runs, 5–30 measurement seconds, and 30–180 boot seconds.")
    try:
        expected = tuple(map(int, args.expect_size.split("x")))
        if len(expected) != 2 or any(n < 640 or n > 3840 for n in expected):
            raise ValueError()
    except ValueError:
        parser.error(
            "Use an expected framebuffer size between 640 and 3840 pixels per axis."
        )
    args.iso = args.iso.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(mode=0o700)  # Refuse overwriting an existing run.
    # Pin both executables: another development session may rebuild the tree.
    binary = args.output / "benchmark-console"
    fixture = args.output / "qa-vm"
    shutil.copy2(
        (ROOT / args.build / "tests/benchmark-console").resolve(strict=True), binary
    )
    shutil.copy2((ROOT / args.build / "tests/qa-vm").resolve(strict=True), fixture)
    name = "pirwa-qa-" + str(uuid.uuid4())
    pool_name = name + "-pool"
    pool = args.output / "pool"
    pool.mkdir(mode=0o700)
    channel = args.output / "guest.sock"
    report = {
        "scope": "Real Fedora live guest, production native console component. Instrumented CPU/copy and GTK submission measurements; excludes physical presentation, GPU completion and main collection UI.",
        "source_commit": run(["git", "rev-parse", "HEAD"]).stdout.strip(),
        "source_dirty": bool(run(["git", "status", "--porcelain"]).stdout),
        "source_sha256": {
            p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
            for p in [
                "src/pirwa-console.c",
                "src/pirwa-display.c",
                "src/pirwa-frame.c",
                "tests/benchmark-console.c",
                "tests/fixtures/qa-display-workload.py",
                "scripts/profile-console.py",
            ]
        },
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "iso": str(args.iso),
        "iso_size": args.iso.stat().st_size,
        "host": platform.platform(),
        "cpu": next(
            x.split(":", 1)[1].strip()
            for x in Path("/proc/cpuinfo").read_text().splitlines()
            if x.startswith("model name")
        ),
        "packages": run(
            [
                "rpm",
                "-q",
                "gtk4",
                "spice-glib",
                "gnome-shell",
                "mutter",
                "qemu-kvm",
                "libvirt-daemon",
            ]
        ).stdout.splitlines(),
        "requested_size": expected,
        "audio_enabled": False,
        "host_load_at_start": os.getloadavg(),
        "allowed_cpus": sorted(os.sched_getaffinity(0)),
        "seconds_per_trial": args.seconds,
        "boot_wait_limit_seconds": args.boot_seconds,
        "name": name,
        "pool_name": pool_name,
        "frame_sample_columns": [
            "paint_wall_us",
            "paint_cpu_us",
            "damage_to_after_paint_us",
        ],
        "runs": [],
        "cleanup": {},
    }

    with args.iso.open("rb") as source:
        report["iso_sha256"] = hashlib.file_digest(source, "sha256").hexdigest()

    def save():
        (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")

    def virsh(*command, check=True):
        return run(["virsh", "-c", args.uri, *command], check=check)

    def baseline():
        data = {}
        for identity in virsh("list", "--all", "--uuid").stdout.split():
            if identity == guest_uuid:
                continue
            data[identity] = {
                "name": virsh("domname", identity).stdout.strip(),
                "state": virsh("domstate", identity).stdout.strip(),
                "xml_sha256": hashlib.sha256(
                    virsh("dumpxml", identity, "--inactive").stdout.encode()
                ).hexdigest(),
            }
        return data

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def action(name, value=None):
        bus.call_sync(
            APP,
            "/io/github/pirwa/ConsoleProfile",
            "org.gtk.Actions",
            "Activate",
            GLib.Variant("(sava{sv})", (name, [] if value is None else [value], {})),
            None,
            Gio.DBusCallFlags.NONE,
            10000,
            None,
        )

    def keys(codes):
        # Setup input goes straight to our disposable QEMU guest. Its timing
        # must not depend on the display client that is under investigation.
        virsh(
            "send-key",
            guest_uuid,
            "--codeset",
            "linux",
            "--holdtime",
            "30",
            *(str(code) for code in codes),
        )
        time.sleep(0.08)

    def text(value):
        letters = dict(
            zip(
                "qwertyuiopasdfghjklzxcvbnm",
                [
                    16,
                    17,
                    18,
                    19,
                    20,
                    21,
                    22,
                    23,
                    24,
                    25,
                    30,
                    31,
                    32,
                    33,
                    34,
                    35,
                    36,
                    37,
                    38,
                    44,
                    45,
                    46,
                    47,
                    48,
                    49,
                    50,
                ],
            )
        )
        mapping = {
            **letters,
            **dict(zip("1234567890", range(2, 12))),
            " ": 57,
            "\n": 28,
            "-": 12,
            "=": 13,
            "/": 53,
            ".": 52,
            ";": 39,
            "'": 40,
        }
        shifted = {"_": "-", ":": ";", '"': "'"}
        for char in value:
            normal = shifted.get(char, char.lower())
            keys(
                ([42] if char.isupper() or char in shifted else []) + [mapping[normal]]
            )

    def interrupt(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    guest_uuid = None
    child = None
    pool_defined = False
    workload_pid = None
    report["baseline_before"] = baseline()
    save()
    try:
        assets = args.output / "guest-assets"
        assets.mkdir()
        for filename in ["qa-channel-agent.py", "qa-display-workload.py"]:
            shutil.copyfile(ROOT / "tests/fixtures" / filename, assets / filename)
        tools_iso = args.output / "tools.iso"
        run(
            [
                "genisoimage",
                "-quiet",
                "-r",
                "-J",
                "-V",
                "PIRWA_QA",
                "-o",
                str(tools_iso),
                str(assets),
            ]
        )
        xml = ET.Element("pool", type="dir")
        ET.SubElement(xml, "name").text = pool_name
        target = ET.SubElement(xml, "target")
        ET.SubElement(target, "path").text = str(pool)
        pool_xml = args.output / "pool.xml"
        ET.ElementTree(xml).write(pool_xml)
        virsh("pool-define", str(pool_xml))
        pool_defined = True
        virsh("pool-start", pool_name)
        guest_uuid = json.loads(
            run(
                [str(fixture), args.uri, "create", name, str(args.iso), pool_name]
            ).stdout
        )["uuid"]
        report["uuid"] = guest_uuid
        save()
        # Creation boots immediately. Stop our fixture before adding its
        # test-only channel and read-only tools CD, then boot the final XML.
        run([str(fixture), args.uri, "stop", guest_uuid])
        devices = [
            "<channel type='unix'><source mode='bind' path='CHANNEL'/><target type='virtio' name='org.pirwa.qa.0'/></channel>",
            "<disk type='file' device='cdrom'><driver name='qemu' type='raw'/><source file='ISO'/><target dev='sdb' bus='sata'/><readonly/></disk>",
        ]
        for i, device in enumerate(devices):
            path = args.output / f"device-{i}.xml"
            # Use parsed attributes below rather than interpolated paths in XML.
            tree = ET.fromstring(device)
            source = tree.find("source")
            source.set(
                "path" if i == 0 else "file", str(channel if i == 0 else tools_iso)
            )
            ET.ElementTree(tree).write(path)
            virsh("attach-device", guest_uuid, str(path), "--config")
        report["guest_xml_sha256"] = hashlib.sha256(
            virsh("dumpxml", guest_uuid, "--inactive").stdout.encode()
        ).hexdigest()
        run([str(fixture), args.uri, "start", guest_uuid])
        with (
            (args.output / "console.stdout.log").open("w") as out,
            (args.output / "console.stderr.log").open("w") as err,
        ):
            env = dict(
                os.environ, GTK_A11Y="test", PULSE_SERVER=f"unix:{runtime}/no-audio"
            )
            child = subprocess.Popen(
                [
                    str(binary),
                    "--uri",
                    args.uri,
                    "--uuid",
                    guest_uuid,
                    "--output",
                    str(args.output / "trials"),
                    "--seconds",
                    str(args.seconds),
                ],
                stdout=out,
                stderr=err,
                env=env,
            )
        print(
            f"Waiting for disposable Fedora guest {name} to connect its SPICE agent",
            flush=True,
        )
        bus.call_sync(
            "org.gnome.Shell",
            "/org/gnome/Shell",
            "org.freedesktop.DBus.Properties",
            "Set",
            GLib.Variant(
                "(ssv)", ("org.gnome.Shell", "OverviewActive", GLib.Variant("b", False))
            ),
            None,
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )
        boot_started = time.monotonic()
        ready = args.output / "trials/ready.json"
        while not ready.exists() or not json.loads(ready.read_text()).get(
            "agent_connected"
        ):
            if (
                child.poll() is not None
                or time.monotonic() - boot_started > args.boot_seconds
            ):
                raise RuntimeError(
                    "Guest agent did not become ready; inspect console.stderr.log"
                )
            time.sleep(0.2)
        report["guest_agent_ready_seconds"] = time.monotonic() - boot_started
        save()
        time.sleep(5)  # Let the live welcome/overview transition finish before input.
        # Fedora live starts in the overview. Escape establishes a known state.
        keys([1])
        keys([56, 60])  # GNOME Run Command (Alt+F2), with no terminal cold start.
        time.sleep(2)
        text(
            "sh -c 'sudo mkdir -p /run/pirwa-qa; sudo mount -o ro /dev/disk/by-label/PIRWA_QA /run/pirwa-qa; sudo env PIRWA_RUN_KVM_TESTS=1 python3 /run/pirwa-qa/qa-channel-agent.py'\n"
        )
        report["guest_agent_identity"] = guest(channel, ["id"], timeout=30).strip()
        report["guest_environment"] = guest(
            channel,
            [
                "sh",
                "-c",
                "uname -a; cat /etc/os-release; rpm -q gtk3 python3-gobject spice-vdagent; loginctl list-sessions --no-legend",
            ],
        )
        # qemu-monitor query is read-only and always targets our disposable UUID.
        cpus = json.loads(
            virsh(
                "qemu-monitor-command", guest_uuid, '{"execute":"query-cpus-fast"}'
            ).stdout
        )["return"]
        thread = cpus[0]["thread-id"]
        qemu_pid = int(
            next(
                x.split(":")[1]
                for x in Path(f"/proc/{thread}/status").read_text().splitlines()
                if x.startswith("Tgid:")
            )
        )

        def qemu_cpu():
            fields = (
                Path(f"/proc/{qemu_pid}/stat").read_text().rsplit(")", 1)[1].split()
            )
            return time.monotonic(), (int(fields[11]) + int(fields[12])) / os.sysconf(
                "SC_CLK_TCK"
            )

        for mode in ["idle", "small", "full"]:
            status = f"/tmp/pirwa-display-{uuid.uuid4()}.json"
            workload_pid = int(
                guest(channel, ["python3", "-c", GUEST_LAUNCH, mode, status])
            )
            time.sleep(3)
            for repeat in range(args.runs):
                for full in [False, True] if repeat % 2 == 0 else [True, False]:
                    variant = "full-copy" if full else "tiles"
                    label = f"{mode}-{repeat}-{variant}"
                    action("full", GLib.Variant("b", full))
                    time.sleep(3)
                    before_guest = json.loads(guest(channel, ["cat", status]))
                    before_qemu = qemu_cpu()
                    action("measure", GLib.Variant("(sb)", (label, full)))
                    trial = args.output / "trials" / (label + ".json")
                    deadline = time.monotonic() + args.seconds + 20
                    while not trial.exists():
                        if child.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("Console trial failed or timed out")
                        time.sleep(0.1)
                    after_qemu = qemu_cpu()
                    after_guest = json.loads(guest(channel, ["cat", status]))
                    result = json.loads(trial.read_text())
                    if result["primary_changes"] or result["suspended"]:
                        raise RuntimeError(
                            "Framebuffer changed or console suspended during a trial"
                        )
                    if (result["width"], result["height"]) != expected or (
                        after_guest["width"] * after_guest["scale"],
                        after_guest["height"] * after_guest["scale"],
                    ) != expected:
                        raise RuntimeError(
                            f"Unexpected guest size: {result['width']}x{result['height']}, workload {after_guest}"
                        )
                    if mode != "idle" and (
                        not result["samples"] or not result["invalidations"]
                    ):
                        raise RuntimeError(
                            "Active workload produced no console updates"
                        )
                    result.update(
                        mode=mode,
                        repeat=repeat,
                        guest_before=before_guest,
                        guest_after=after_guest,
                        qemu_observer_wall_seconds=after_qemu[0] - before_qemu[0],
                        qemu_cpu_seconds=after_qemu[1] - before_qemu[1],
                    )
                    report["runs"].append(result)
                    save()
                    print(
                        f"{mode} {repeat + 1}/{args.runs} {variant}: {result['cpu_us'] * 100 / result['wall_us']:.2f}% CPU, {len(result['samples']) * 1e6 / result['wall_us']:.1f} GTK updates/s, {result['copied_bytes'] / 1024**2:.1f} MiB copied",
                        flush=True,
                    )
            guest(channel, ["kill", "-TERM", "--", f"-{workload_pid}"])
            deadline = time.monotonic() + 5
            while not json.loads(guest(channel, ["cat", status]))["complete"]:
                if time.monotonic() > deadline:
                    raise RuntimeError("Guest drawing process did not stop")
                time.sleep(0.2)
            workload_pid = None
        report["measurements_completed"] = True
    except BaseException as error:
        report["error"] = str(error) or type(error).__name__
        if guest_uuid:
            try:
                capture = run(
                    [
                        "virsh",
                        "-c",
                        args.uri,
                        "screenshot",
                        guest_uuid,
                        str(args.output / "failure-guest.png"),
                    ],
                    check=False,
                    timeout=10,
                )
                report["failure_capture"] = capture.stderr or capture.stdout
            except subprocess.TimeoutExpired:
                report["failure_capture"] = "Capture timed out"
        raise
    finally:
        if child and child.poll() is None:
            try:
                action("quit")
                child.wait(timeout=10)
            except (GLib.Error, subprocess.TimeoutExpired):
                child.terminate()
                child.wait(timeout=10)
        if guest_uuid:
            for operation in ["stop", "remove"]:
                result = run(
                    [str(fixture), args.uri, operation, guest_uuid], check=False
                )
                report["cleanup"][operation] = {
                    "code": result.returncode,
                    "stderr": result.stderr,
                }
        if pool_defined and not any(pool.iterdir()):
            report["cleanup"]["pool_destroy"] = virsh(
                "pool-destroy", pool_name, check=False
            ).returncode
            report["cleanup"]["pool_undefine"] = virsh(
                "pool-undefine", pool_name, check=False
            ).returncode
        report["cleanup"]["remaining_pool_files"] = [x.name for x in pool.iterdir()]
        report["cleanup"]["domain_absent"] = bool(
            virsh("dominfo", name, check=False).returncode
        )
        report["baseline_after"] = baseline()
        report["cleanup"]["pool_absent"] = bool(
            virsh("pool-info", pool_name, check=False).returncode
        )
        report["cleanup"]["baseline_unchanged"] = (
            report["baseline_before"] == report["baseline_after"]
        )
        save()
    if (
        not report["cleanup"]["domain_absent"]
        or not report["cleanup"]["pool_absent"]
        or report["cleanup"]["remaining_pool_files"]
        or not report["cleanup"]["baseline_unchanged"]
    ):
        raise RuntimeError(
            "Cleanup/baseline verification needs attention; inspect results.json"
        )
    # Summarize paired results; all raw trials remain in results.json.
    report["summary"] = []
    for mode in ["idle", "small", "full"]:
        for full in [False, True]:
            trials = [
                r
                for r in report["runs"]
                if r["mode"] == mode and r["force_full"] == full
            ]
            values = sorted(s[0] for r in trials for s in r["samples"])
            report["summary"].append(
                {
                    "mode": mode,
                    "force_full": full,
                    "median_cpu_percent": statistics.median(
                        r["cpu_us"] * 100 / r["wall_us"] for r in trials
                    ),
                    "median_updates_per_second": statistics.median(
                        len(r["samples"]) * 1e6 / r["wall_us"] for r in trials
                    ),
                    "paint_wall_p95_us": values[
                        min(len(values) - 1, int(len(values) * 0.95))
                    ]
                    if values
                    else None,
                }
            )
    report["completed"] = True
    report["host_load_at_end"] = os.getloadavg()
    save()


if __name__ == "__main__":
    main()
