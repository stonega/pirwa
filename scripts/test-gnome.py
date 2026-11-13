#!/usr/bin/env python3
"""Run Pirwa UI QA in a disposable headless GNOME Shell desktop."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import subprocess
import sys
import tempfile
import time

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
    shell_args = ["gnome-shell", "--headless", "--wayland", "--no-x11",
                  "--virtual-monitor", "1280x800@60",
                  "--wayland-display", socket.name]
    report = {
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True)),
        "gnome_shell": subprocess.check_output(
            ["gnome-shell", "--version"], text=True).strip(),
        "shell_command": shell_args,
        "isolation": "Private D-Bus, runtime/config/data/cache/state directories; memory GSettings",
        "scope": ("Custom command in an isolated desktop; inspect the command's own results"
                  if args.command else
                  "Programmed production GTK UI actions; not physical input, screen reader, or guest console certification"),
        "cases": [],
        "passed": False,
    }
    env = dict(os.environ, WAYLAND_DISPLAY=socket.name, GDK_BACKEND="wayland")

    def execute(name, command):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True,
                                text=True, timeout=args.command_timeout)
        (output / f"{name}.stdout.log").write_text(result.stdout)
        (output / f"{name}.stderr.log").write_text(result.stderr)
        case = {"name": name, "command": command, "returncode": result.returncode}
        try:
            case["metrics"] = json.loads(result.stdout)
        except json.JSONDecodeError:
            pass
        report["cases"].append(case)
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {output}")
        print(f"PASS {name}", flush=True)

    with (output / "gnome-shell.log").open("w") as log:
        shell = subprocess.Popen(shell_args, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            # Wait for compositor initialization, not merely socket creation.
            deadline = time.monotonic() + 30
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
                binary = str((ROOT / args.build / "tests/test-ui").resolve())
                execute("light", [binary, "--exercise", "--snapshot", str(output / "light.png")])
                execute("large-dark", [binary, "--exercise", "--large-text", "--dark",
                                       "--width", "720", "--snapshot", str(output / "large-dark.png")])
                fixture = runpy.run_path(str(ROOT / "scripts/profile-ui.py"))["fixture"]
                uri = fixture(Path(args.session_root), 0)
                execute("empty", [binary, "--exercise", "--uri", uri,
                                  "--snapshot", str(output / "empty.png")])
                execute("boxes", [binary, "--boxes-exercise", "--snapshot", str(output / "boxes.png")])
                execute("boxes-large-dark", [binary, "--boxes-exercise", "--large-text", "--dark",
                                             "--width", "720", "--snapshot", str(output / "boxes-large-dark.png")])
            report["passed"] = True
        except Exception as error:
            report["error"] = str(error)
            print(str(error), file=sys.stderr)
        finally:
            shell.terminate()
            try:
                shell.wait(timeout=10)
            except subprocess.TimeoutExpired:
                shell.kill()
                shell.wait()
            report["shell_exitcode"] = shell.returncode
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/qa/gnome")
    parser.add_argument("--session-root", help=argparse.SUPPRESS)
    parser.add_argument("--command-timeout", type=int, default=600,
                        help="Seconds allowed for each command (1–7200; default: 600)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Optional command after --, run instead of the default UI matrix")
    args = parser.parse_args()
    if not 1 <= args.command_timeout <= 7200:
        parser.error("Use a command timeout between 1 and 7200 seconds")
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.session_root:
        return session(args)
    for program in ("gnome-shell", "dbus-run-session"):
        if not shutil.which(program):
            parser.error(f"{program} is required; install the GNOME desktop test dependencies")

    with tempfile.TemporaryDirectory(prefix="pirwa-gnome-") as directory:
        base = Path(directory)
        env = os.environ.copy()
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(key, None)
        for suffix in ("RUNTIME_DIR", "CONFIG_HOME", "DATA_HOME", "CACHE_HOME", "STATE_HOME"):
            path = base / suffix.split("_", 1)[0].lower()
            path.mkdir(mode=0o700)
            env[f"XDG_{suffix}"] = str(path)
        env.update(GSETTINGS_BACKEND="memory", XDG_CURRENT_DESKTOP="GNOME",
                   XDG_SESSION_TYPE="wayland", GVFS_DISABLE_FUSE="1", LC_ALL="C")
        env["PIRWA_QA_ISOLATED"] = "1"
        command = ["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                   "--build", args.build, "--output", str(args.output),
                   "--session-root", directory]
        command += ["--command-timeout", str(args.command_timeout)]
        if args.command:
            command += ["--", *args.command]
        try:
            with (args.output / "session.log").open("w") as log:
                result = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        finally:
            try:
                terminate_session_processes(base, args.output)
            finally:
                unmount_session_filesystems(base, args.output)
        code = result.returncode
    print(f"{'PASS' if code == 0 else 'FAIL'} virtual GNOME desktop: {args.output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
