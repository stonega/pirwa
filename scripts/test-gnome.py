#!/usr/bin/env python3
"""Run Pirwa UI QA with private session state on virtual or physical GNOME."""
import argparse
import hashlib
import json
import os
import re
import runpy
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def session_processes(runtime):
    """Only this user's processes carrying this run's exact private runtime."""
    processes = {}
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            env = dict(item.split('=', 1) for item in (proc / 'environ').read_text().split('\0')
                       if '=' in item)
            if env.get('XDG_RUNTIME_DIR') == str(runtime):
                processes[int(proc.name)] = (proc / 'comm').read_text().strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return processes


def terminate_session_processes(base, output):
    """Some desktop helpers survive losing their bus; reap only our fixture."""
    runtime = base / 'runtime'
    signalled = []
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid, name in session_processes(runtime).items():
            try:
                descriptor = os.pidfd_open(pid)
                try:
                    if pid in session_processes(runtime):
                        signal.pidfd_send_signal(descriptor, sig)
                        signalled.append({'pid': pid, 'name': name, 'signal': sig.name})
                finally:
                    os.close(descriptor)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 3
        while session_processes(runtime) and time.monotonic() < deadline:
            time.sleep(.05)
        if not session_processes(runtime):
            break
    remaining = session_processes(runtime)
    (output / 'process-cleanup.json').write_text(json.dumps(
        {'signalled': signalled, 'remaining': remaining}, indent=2) + '\n')
    if remaining:
        raise RuntimeError(f'Test desktop processes remain: {remaining}')


def unmount_session_filesystems(base, output):
    """Portal services may outlive dbus-run-session briefly during teardown."""
    deadline = time.monotonic() + 10
    with (output / "cleanup.log").open("w") as log:
        while True:
            mounts = []
            for line in Path("/proc/self/mountinfo").read_text().splitlines():
                fields = line.split()
                target = fields[4]
                filesystem = fields[fields.index("-") + 1]
                if target.startswith(str(base) + "/") and filesystem.startswith("fuse"):
                    mounts.append(target)
            if not mounts:
                return
            if time.monotonic() >= deadline or not shutil.which("fusermount3"):
                raise RuntimeError(f"Cannot clean up test-session FUSE mounts: {mounts}")
            for target in sorted(mounts, key=len, reverse=True):
                # Only FUSE mounts beneath this run's private temporary directory.
                subprocess.run(["fusermount3", "-u", "-z", target], stdout=log,
                               stderr=subprocess.STDOUT, check=False)
            time.sleep(0.05)


def session(args):
    output = args.output
    runtime = Path(args.session_root) / "runtime"
    socket = runtime / "pirwa-qa-wayland"
    shell_args = None if args.physical else ["gnome-shell", "--headless", "--wayland", "--no-x11",
                  "--virtual-monitor", args.monitor,
                  "--wayland-display", socket.name]
    report = {
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True)),
        "gnome_shell": subprocess.check_output(
            ["gnome-shell", "--version"], text=True).strip(),
        "shell_command": shell_args,
        "display_kind": "physical" if args.physical else "virtual",
        "graphics_environment": {key: os.environ[key] for key in
                                 ["GSK_RENDERER", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "DRI_PRIME",
                                  "MESA_VK_DEVICE_SELECT", "__NV_PRIME_RENDER_OFFLOAD"] if key in os.environ},
        "isolation": "Private D-Bus, runtime/config/data/cache/state directories; memory GSettings",
        "scope": ("Custom command with isolated session state; inspect the command's own results"
                  if args.command else
                  "Programmed GTK actions and focus traversal; not physical input, screen reader, or guest console certification"),
        "repetitions": args.repeat,
        "cases": [],
        "passed": False,
    }
    env = dict(os.environ, GDK_BACKEND="wayland")
    if not args.physical:
        env["WAYLAND_DISPLAY"] = socket.name

    def execute(name, command):
        started = time.monotonic()
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        timed_out = False
        try:
            stdout, stderr = child.communicate(timeout=args.command_timeout)
        except subprocess.TimeoutExpired:
            # Real-guest commands own resources beyond this process. Let their
            # SIGTERM/finally cleanup run before forcing an unresponsive child.
            timed_out = True
            child.terminate()
            try:
                stdout, stderr = child.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                child.kill()
                stdout, stderr = child.communicate()
        (output / f"{name}.stdout.log").write_text(stdout)
        (output / f"{name}.stderr.log").write_text(stderr)
        case = {"name": name, "command": command, "returncode": child.returncode,
                "elapsed_seconds": round(time.monotonic() - started, 3)}
        try:
            case["metrics"] = json.loads(stdout)
        except json.JSONDecodeError:
            pass
        report["cases"].append(case)
        if timed_out:
            case["timed_out"] = True
            raise RuntimeError(f"{name} timed out after {args.command_timeout}s; see {output}")
        if child.returncode:
            raise RuntimeError(f"{name} failed ({child.returncode}); see {output}")
        print(f"PASS {name}", flush=True)

    with (output / "gnome-shell.log").open("w") as log:
        shell = (subprocess.Popen(shell_args, stdout=log, stderr=subprocess.STDOUT,
                                  start_new_session=True) if shell_args else None)
        try:
            # Wait for compositor initialization, not merely socket creation.
            deadline = time.monotonic() + 30
            if shell:
                while time.monotonic() < deadline:
                    if shell.poll() is not None:
                        raise RuntimeError(f"GNOME Shell exited ({shell.returncode})")
                    if socket.exists() and "GNOME Shell started" in (output / "gnome-shell.log").read_text():
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError("GNOME Shell startup timed out")

            if args.command:
                execute("command", args.command)
            else:
                schemas = (ROOT / args.build / "data").resolve()
                compiled = schemas / "gschemas.compiled"
                if not compiled.is_file():
                    raise RuntimeError("Build the matching GSettings schemas before desktop QA")
                env["GSETTINGS_SCHEMA_DIR"] = str(schemas)
                report["schema_sha256"] = hashlib.sha256(compiled.read_bytes()).hexdigest()
                for repetition in range(args.repeat):
                    prefix = f"run-{repetition + 1:02d}-" if args.repeat > 1 else ""
                    matrix_output = output / f"run-{repetition + 1:02d}" if args.repeat > 1 else output
                    matrix_output.mkdir(parents=True, exist_ok=True)
                    binary = str((ROOT / args.build / "tests/test-ui").resolve())
                    execute(prefix + "light", [binary, "--exercise", "--snapshot",
                                              str(matrix_output / "light.png")])
                    execute(prefix + "large-dark", [binary, "--exercise", "--large-text", "--dark",
                                                   "--width", "720", "--snapshot",
                                                   str(matrix_output / "large-dark.png")])
                    fixture = runpy.run_path(str(ROOT / "scripts/profile-ui.py"))["fixture"]
                    uri = fixture(Path(args.session_root), 0)
                    execute(prefix + "empty", [binary, "--exercise", "--uri", uri,
                                              "--snapshot", str(matrix_output / "empty.png")])
                    execute(prefix + "boxes", [binary, "--boxes-exercise", "--snapshot",
                                              str(matrix_output / "boxes.png")])
                    execute(prefix + "boxes-large-dark", [binary, "--boxes-exercise", "--large-text", "--dark",
                                                         "--width", "720", "--snapshot",
                                                         str(matrix_output / "boxes-large-dark.png")])
                    execute(prefix + "boxes-deferred", [binary, "--boxes-deferred-exercise", "--snapshot",
                                                       str(matrix_output / "boxes-deferred.png")])
                    execute(prefix + "boxes-deferred-large-dark",
                            [binary, "--boxes-deferred-exercise", "--large-text", "--dark", "--width", "720",
                             "--snapshot", str(matrix_output / "boxes-deferred-large-dark.png")])
                    viewer = str((ROOT / args.build / "tests/test-console-ui").resolve())
                    execute(prefix + "viewer", [viewer, "--output", str(matrix_output / "viewer")])
                    execute(prefix + "viewer-large-dark", [viewer, "--large-text", "--dark", "--width", "720",
                                                           "--output", str(matrix_output / "viewer-large-dark")])
                    lifetime = str((ROOT / args.build / "tests/test-ui-lifetime").resolve())
                    execute(prefix + "lifetime", [lifetime])
            report["passed"] = True
        except Exception as error:  # noqa: BLE001 - preserve diagnostics before teardown
            report["error"] = str(error)
            print(str(error), file=sys.stderr)
        finally:
            if shell:
                shell.terminate()
                try:
                    shell.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    shell.kill()
                    shell.wait()
            report["shell_exitcode"] = shell.returncode if shell else None
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--physical", action="store_true",
                        help="Use the caller's unlocked Wayland desktop with private bus and state; do not start a compositor")
    parser.add_argument("--repeat", type=int, default=1,
                        help="Repeat the standard matrix 1–20 times in one desktop")
    parser.add_argument("--monitor", default="1280x800@60",
                        help="Virtual monitor WxH@Hz (default: 1280x800@60)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/qa/gnome")
    parser.add_argument("--session-root", help=argparse.SUPPRESS)
    parser.add_argument("--command-timeout", type=int, default=600,
                        help="Seconds allowed for each command (1–7200; default: 600)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Optional command after --, run instead of the default UI matrix")
    args = parser.parse_args()
    monitor = re.fullmatch(r"(\d+)x(\d+)@(\d+)", args.monitor)
    if not monitor or not (640 <= int(monitor[1]) <= 3840
                           and 480 <= int(monitor[2]) <= 2160 and 20 <= int(monitor[3]) <= 120):
        parser.error("Use a monitor from 640x480 to 3840x2160 at 20–120 Hz")
    if not 1 <= args.command_timeout <= 7200:
        parser.error("Use a command timeout between 1 and 7200 seconds")
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not 1 <= args.repeat <= 20 or (args.command and args.repeat != 1):
        parser.error("Use --repeat 1–20 with the standard matrix only")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.session_root:
        return session(args)
    programs = ["gnome-shell", "dbus-run-session"]
    if args.physical:
        programs.append("gdbus")
    for program in programs:
        if not shutil.which(program):
            parser.error(f"{program} is required; install the GNOME desktop test dependencies")

    display_socket = None
    if args.physical:
        display = os.environ.get("WAYLAND_DISPLAY")
        host_runtime = os.environ.get("XDG_RUNTIME_DIR")
        if not display or (not Path(display).is_absolute() and not host_runtime):
            parser.error("Physical QA needs WAYLAND_DISPLAY and its host XDG_RUNTIME_DIR")
        display_socket = Path(display) if Path(display).is_absolute() else Path(host_runtime) / display
        display_socket = display_socket.resolve()
        if not display_socket.exists() or not stat.S_ISSOCK(display_socket.stat().st_mode):
            parser.error("The caller's Wayland display socket is unavailable")
        locked = subprocess.run(["gdbus", "call", "--session", "--dest", "org.gnome.ScreenSaver",
                                 "--object-path", "/org/gnome/ScreenSaver", "--method",
                                 "org.gnome.ScreenSaver.GetActive"], text=True,
                                capture_output=True, timeout=10, check=False)
        if locked.returncode or locked.stdout.strip() != "(false,)":
            parser.error("Physical QA needs an unlocked GNOME desktop; leave test windows unobscured")

    with tempfile.TemporaryDirectory(prefix="pirwa-gnome-") as directory:
        base = Path(directory)
        env = os.environ.copy()
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(key, None)
        if display_socket:
            # An absolute socket path connects to the host compositor while all
            # services and cleanup retain this run's private runtime marker.
            env["WAYLAND_DISPLAY"] = str(display_socket)
        for suffix in ("RUNTIME_DIR", "CONFIG_HOME", "DATA_HOME", "CACHE_HOME", "STATE_HOME"):
            path = base / suffix.split("_", 1)[0].lower()
            path.mkdir(mode=0o700)
            env[f"XDG_{suffix}"] = str(path)
        env.update(GSETTINGS_BACKEND="memory", XDG_CURRENT_DESKTOP="GNOME",
                   XDG_SESSION_TYPE="wayland", GVFS_DISABLE_FUSE="1", LC_ALL="C")
        env["PIRWA_QA_ISOLATED"] = "1"
        command = ["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                   "--build", args.build, "--monitor", args.monitor, "--output", str(args.output),
                   "--session-root", directory]
        command += ["--command-timeout", str(args.command_timeout), "--repeat", str(args.repeat)]
        if args.physical:
            command.append("--physical")
        if args.command:
            command += ["--", *args.command]
        try:
            with (args.output / "session.log").open("w") as log:
                result = subprocess.run(command, check=False, cwd=ROOT, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        finally:
            try:
                terminate_session_processes(base, args.output)
            finally:
                unmount_session_filesystems(base, args.output)
        code = result.returncode
    kind = "physical" if args.physical else "virtual"
    print(f"{'PASS' if code == 0 else 'FAIL'} {kind} GNOME desktop: {args.output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
