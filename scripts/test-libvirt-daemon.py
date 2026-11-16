#!/usr/bin/env python3
"""Exercise Pirwa against a private virtqemud, without defining or booting a VM."""

import argparse
import base64
import hashlib
import json
import os
import selectors
import shutil
import signal
import stat
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def pin_client(build, output):
    pinned = output / 'test-backend-daemon'
    shutil.copy2(build / 'tests/test-backend-daemon', pinned)
    schemas = output / 'schemas'
    schemas.mkdir()
    shutil.copy2(build / 'data/gschemas.compiled', schemas / 'gschemas.compiled')
    return pinned, schemas


def interrupted(signum, _frame):
    # Allow the fixture's finally blocks to run if its launcher terminates it.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise KeyboardInterrupt(f'Interrupted by signal {signum}')


def private_processes(runtime):
    """Match only this user's processes carrying the exact private runtime."""
    found = {}
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            env = dict(part.split('=', 1) for part in (proc / 'environ').read_text().split('\0')
                       if '=' in part)
            if env.get('XDG_RUNTIME_DIR') == str(runtime):
                found[int(proc.name)] = (proc / 'comm').read_text().strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return found


def reap_helpers(runtime):
    signalled = []
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid, name in private_processes(runtime).items():
            try:
                descriptor = os.pidfd_open(pid)
                try:
                    if pid in private_processes(runtime):
                        signal.pidfd_send_signal(descriptor, sig)
                        signalled.append({'pid': pid, 'name': name, 'signal': sig.name})
                finally:
                    os.close(descriptor)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 3
        while private_processes(runtime) and time.monotonic() < deadline:
            time.sleep(.05)
    remaining = private_processes(runtime)
    return {'signalled': signalled, 'remaining': remaining}


class Daemon:
    def __init__(self, base, env, output, activation=False):
        self.env = env
        self.output = output
        self.activation = activation
        self.process = None
        self.generations = []
        self.socket = base / 'runtime/libvirt/virtqemud-sock'
        self.config = base / 'config/libvirt/virtqemud.conf'
        self.config.parent.mkdir()
        (self.config.parent / 'qemu.conf').write_text('')
        self.config.write_text('auth_unix_rw = "none"\nunix_sock_rw_perms = "0600"\n')
        self.command = ['virtqemud', '--config', str(self.config),
                        '--pid-file', str(base / 'runtime/daemon.pid')]
        if activation:
            self.socket.parent.mkdir(mode=0o700)
            # Match the descriptor name supplied by virtqemud.socket. Explicitly
            # forward only the fixture environment to the activated daemon.
            self.command = ['systemd-socket-activate', '--listen=' + str(self.socket),
                            '--fdname=virtqemud.socket',
                            *['--setenv=' + key for key in env], *self.command]

    def start(self):
        if self.process is not None:
            raise RuntimeError('Private daemon is already running')
        with (self.output / f'daemon-{len(self.generations)}.log').open('w') as log:
            self.process = subprocess.Popen(self.command, env=self.env, stdout=log,
                                            stderr=subprocess.STDOUT, start_new_session=True,
                                            umask=0o077)
        self.generations.append({'pid': self.process.pid, 'command': self.command})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f'Private virtqemud exited ({self.process.returncode})')
            if self.socket.exists() and stat.S_ISSOCK(self.socket.lstat().st_mode):
                self.generations[-1]['socket_inode'] = self.socket.stat().st_ino
                return
            time.sleep(.05)
        raise RuntimeError('Private virtqemud socket did not appear')

    def waiting_for_connection(self):
        executable = Path(f'/proc/{self.process.pid}/exe').resolve().name
        if executable != 'systemd-socket-activate':
            raise RuntimeError(f'Daemon started before its first client: {executable}')
        self.generations[-1]['waiting_before_first_connection'] = True

    def activated(self):
        if not self.activation:
            return
        executable = Path(f'/proc/{self.process.pid}/exe').resolve().name
        generation = self.generations[-1]
        if (executable != 'virtqemud' or self.process.poll() is not None
                or self.socket.stat().st_ino != generation['socket_inode']):
            raise RuntimeError('Client did not activate virtqemud with the listening socket')
        generation['activation_confirmed'] = True

    def stop(self, crash=False):
        if self.process is None:
            return
        process = self.process
        if process.poll() is None:
            if crash:
                process.kill()
            else:
                process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        self.generations[-1]['exitcode'] = process.returncode
        self.generations[-1]['crash_requested'] = crash
        self.process = None
        # SIGKILL leaves its private socket behind. Remove only that fixture's
        # endpoints after its owner has exited, before starting a replacement.
        for path in self.socket.parent.glob('virtqemud*'):
            try:
                if stat.S_ISSOCK(path.lstat().st_mode):
                    path.unlink()
            except FileNotFoundError:
                pass


class Client:
    def __init__(self, binary, env, output):
        self.events = []
        self.buffer = b''
        self.next_id = 0
        self.stderr = (output / 'client.stderr.log').open('w')
        self.log = (output / 'client.events.jsonl').open('w')
        self.process = subprocess.Popen([str(binary)], env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def send(self, command):
        self.process.stdin.write((command + '\n').encode())
        self.process.stdin.flush()

    def receive(self, predicate, since=0, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            for event in self.events[since:]:
                if predicate(event):
                    return event
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError('Backend daemon QA timed out waiting for an event')
            for key, _ in self.selector.select(min(remaining, .5)):
                data = os.read(key.fd, 65536)
                if not data:
                    raise RuntimeError(f'Backend client closed its protocol ({self.process.poll()})')
                self.buffer += data
                while b'\n' in self.buffer:
                    line, self.buffer = self.buffer.split(b'\n', 1)
                    event = json.loads(line)
                    if 'message_base64' in event:
                        event['message'] = base64.b64decode(event.pop('message_base64')).decode()
                    event['observed_monotonic'] = time.monotonic()
                    self.events.append(event)
                    self.log.write(json.dumps(event) + '\n')
                    self.log.flush()

    def request(self, command):
        self.next_id += 1
        self.send(command)
        return self.receive(lambda event: event.get('event') == 'reply'
                            and event['id'] == self.next_id)

    def close(self):
        try:
            if self.process.poll() is None:
                self.send('quit')
                self.receive(lambda event: event['event'] == 'joined', timeout=5)
                self.process.wait(timeout=5)
        except (BrokenPipeError, RuntimeError, subprocess.TimeoutExpired):
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=5)
        finally:
            self.selector.close()
            self.process.stdin.close()
            self.process.stdout.close()
            self.log.close()
            self.stderr.close()


def healthy(event):
    if not event.get('ok') or event.get('machines') != 0 or not event.get('host') or not event.get('pools'):
        raise RuntimeError(f'Expected an empty healthy private inventory with metadata: {event}')


def exercise(client, daemon, report):
    def passed(name):
        report['checks'].append(name)
        print(f'PASS {name}', flush=True)

    client.receive(lambda event: event['event'] == 'ready')
    initial = client.request('refresh')
    if daemon.activation:
        healthy(initial)
        daemon.activated()
        passed('default-session-uri-cold-socket-activation')
    else:
        if initial['ok'] or initial['cancelled']:
            raise RuntimeError('Missing private daemon did not produce a connection failure')
        since = len(client.events)
        daemon.start()
        recovered = client.receive(lambda event: event['event'] == 'inventory', since)
        healthy(recovered)
        if recovered['serial'] <= initial['serial']:
            raise RuntimeError('Recovery did not advance the completion serial')
        passed('startup-unavailable-automatic-recovery')

    for crash, label in [(False, 'graceful-restart'), (True, 'crash-restart')]:
        since = len(client.events)
        daemon.stop(crash=crash)
        failure = client.receive(lambda event: event['event'] == 'error', since)
        since = len(client.events)
        daemon.start()
        recovered = client.receive(lambda event: event['event'] == 'inventory', since)
        healthy(recovered)
        daemon.activated()
        if recovered['serial'] <= failure['serial']:
            raise RuntimeError('Daemon replacement did not advance the completion serial')
        passed(label + '-automatic-recovery')

    daemon.socket.chmod(0)
    try:
        denied = client.request('reconnect')
        if denied['ok'] or 'Permission denied' not in denied.get('message', ''):
            raise RuntimeError(f'Private socket permissions were not enforced: {denied}')
    finally:
        daemon.socket.chmod(0o600)
    since = len(client.events)
    recovered = client.receive(lambda event: event['event'] == 'inventory', since)
    healthy(recovered)
    passed('socket-permission-denial-automatic-recovery')

    since = len(client.events)
    daemon.stop()
    client.receive(lambda event: event['event'] == 'error', since)
    since = len(client.events)
    client.send('ping')
    client.receive(lambda event: event['event'] == 'pong', since)
    client.send('stop')
    client.receive(lambda event: event['event'] == 'stopped', since)
    cancelled = client.request('refresh')
    if cancelled['ok'] or not cancelled['cancelled']:
        raise RuntimeError('Stopped backend accepted another inventory request')
    passed('main-loop-and-stop-during-real-backoff')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/qa/libvirt-daemon')
    parser.add_argument('--socket-activation', action='store_true',
                        help='Use qemu:///session and activate the private daemon on connection')
    args = parser.parse_args()
    if os.environ.get('PIRWA_RUN_DAEMON_TESTS') != '1' or os.geteuid() == 0:
        parser.error('Use PIRWA_RUN_DAEMON_TESTS=1 as a regular user; no root is needed')
    if not shutil.which('virtqemud'):
        parser.error('virtqemud and its QEMU driver are required')
    if args.socket_activation and not shutil.which('systemd-socket-activate'):
        parser.error('systemd-socket-activate is required for activation checks')
    binary = (args.build / 'tests/test-backend-daemon').resolve()
    if not binary.is_file():
        parser.error('Build test-backend-daemon first')
    if not (args.build / 'data/gschemas.compiled').is_file():
        parser.error('Run meson compile -C BUILD to build matching GSettings schemas')
    signal.signal(signal.SIGTERM, interrupted)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    pinned, schemas = pin_client(args.build, args.output)
    report = {'passed': False, 'checks': [], 'scope': 'Private unprivileged virtqemud; no domain, disk or network creation; actual transport and production retry delays',
              'binary_sha256': hashlib.sha256(pinned.read_bytes()).hexdigest(),
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for folder in ['src', 'tests'] for p in (ROOT / folder).glob('*.[ch]')},
              'virtqemud_version': subprocess.check_output(['virtqemud', '--version'], text=True).strip()}
    report['source_sha256']['scripts/test-libvirt-daemon.py'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report['schema_sha256'] = hashlib.sha256((schemas / 'gschemas.compiled').read_bytes()).hexdigest()
    report['socket_activation'] = args.socket_activation
    report['connection_mode'] = 'qemu:///session' if args.socket_activation else 'explicit private Unix socket'
    if args.socket_activation:
        report['systemd_version'] = subprocess.check_output(['systemd-socket-activate', '--version'], text=True).splitlines()[0]
    with tempfile.TemporaryDirectory(prefix='pirwa-libvirt-', dir='/tmp') as directory:
        base = Path(directory)
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LIBVIRT_AUTOSTART': '0'}
        for key, suffix in [('RUNTIME_DIR', 'runtime'), ('CONFIG_HOME', 'config'),
                            ('DATA_HOME', 'data'), ('CACHE_HOME', 'cache'), ('STATE_HOME', 'state')]:
            path = base / suffix
            path.mkdir(mode=0o700)
            env['XDG_' + key] = str(path)
        daemon = Daemon(base, env, args.output, activation=args.socket_activation)
        client_env = dict(env, PIRWA_RUN_DAEMON_TESTS='1', PIRWA_DAEMON_TEST_ROOT=str(base))
        client_env.update(GSETTINGS_SCHEMA_DIR=str(schemas), GSETTINGS_BACKEND='memory')
        if args.socket_activation:
            client_env['PIRWA_DAEMON_TEST_SOCKET_ACTIVATION'] = '1'
        for key in ['LD_LIBRARY_PATH', 'ASAN_OPTIONS', 'UBSAN_OPTIONS', 'LSAN_OPTIONS', 'DEBUGINFOD_URLS']:
            if key in os.environ:
                client_env[key] = os.environ[key]
        client = None
        try:
            if args.socket_activation:
                daemon.start()
                daemon.waiting_for_connection()
            client = Client(pinned, client_env, args.output)
            exercise(client, daemon, report)
            report['passed'] = True
        except (Exception, KeyboardInterrupt) as error:  # noqa: BLE001 - always tear down the fixture
            report['error'] = str(error) or type(error).__name__
        finally:
            if client:
                client.close()
                report['client_exitcode'] = client.process.returncode
                report['events'] = client.events
            daemon.stop()
            report['daemon_generations'] = daemon.generations
            report['process_cleanup'] = reap_helpers(base / 'runtime')
            report['private_domain_files'] = [str(p.relative_to(base)) for p in base.glob('config/libvirt/qemu/*.xml')]
            report['private_storage_files'] = [str(p.relative_to(base)) for p in base.glob('config/libvirt/storage/*.xml')]
            if (report.get('client_exitcode') != 0 or report['process_cleanup']['remaining']
                    or report['private_domain_files'] or report['private_storage_files']):
                report['passed'] = False
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    report['private_root_removed'] = not base.exists()
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{'PASS' if report['passed'] else 'FAIL'} private libvirt daemon: {args.output}")
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
