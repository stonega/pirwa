#!/usr/bin/env python3
"""Opt-in input/screenshot controller for test-gnome.py's private desktop.

Does not create guests automatically. The caller owns disposable-guest cleanup.
Requires Python GObject introspection, PipeWire/Pulse, WirePlumber, and GStreamer.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parent.parent
PIPEWIRE_CONFIG = """
context.properties = { core.daemon = true core.name = pipewire-0 }
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-client-device }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
context.objects = [
    { factory = spa-node-factory args = {
        factory.name = support.node.driver node.name = Dummy-Driver
        node.group = pipewire.dummy priority.driver = 20000
    } }
]
"""


class Desktop:
    def __init__(self, args):
        self.args = args
        self.children = []
        self.manager = None
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.remote = None
        self.stream = None

    def call(self, destination, path, interface, method, signature=None, values=()):
        return self.bus.call_sync(destination, path, interface, method,
                                  GLib.Variant(signature, values) if signature else None,
                                  None, Gio.DBusCallFlags.NONE, 20000, None).unpack()

    def input(self, method, signature=None, values=()):
        return self.call('org.gnome.Mutter.RemoteDesktop', self.remote,
                         'org.gnome.Mutter.RemoteDesktop.Session', method, signature, values)

    def spawn(self, name, command, env=None):
        with (self.args.output / f'{name}.log').open('w') as log:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append(child)
        return child

    def run(self, command):
        # A console deliberately outlives its launching qa-vm process. Pipes
        # inherited by that viewer would keep communicate() waiting for EOF.
        with tempfile.TemporaryFile(mode='w+t') as output, tempfile.TemporaryFile(mode='w+t') as errors:
            result = subprocess.run(command, stdout=output, stderr=errors, timeout=120)
            output.seek(0)
            errors.seek(0)
            if result.returncode:
                raise RuntimeError(f'{command[0]} failed: {errors.read()}')
            return output.read().strip()

    def start(self):
        # Only our private bus; DO_NOT_QUEUE never replaces an existing owner.
        owned = self.call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                          'org.freedesktop.DBus', 'RequestName', '(su)',
                          ('org.gnome.SettingsDaemon.MediaKeys', 4))[0]
        if owned != 1:
            raise RuntimeError('Screenshot fixture name is already owned')
        config = Path(os.environ['XDG_CONFIG_HOME']) / 'pirwa-qa.conf'
        config.write_text(PIPEWIRE_CONFIG)
        self.spawn('pipewire', ['pipewire', '-c', str(config)])
        pulse = self.spawn('pulse', ['pipewire-pulse'])
        self.spawn('policy', ['wireplumber', '--profile', 'policy'])
        deadline = time.monotonic() + 15
        while not Path(os.environ['XDG_RUNTIME_DIR'], 'pulse/native').exists():
            if pulse.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Private audio service failed to start')
            time.sleep(.05)
        self.run(['pactl', 'load-module', 'module-null-sink', 'sink_name=pirwa_qa'])
        self.run(['pactl', 'set-default-sink', 'pirwa_qa'])
        self.run(['pactl', 'set-default-source', 'pirwa_qa.monitor'])
        sinks = self.run(['pactl', 'list', 'short', 'sinks'])
        if any('pirwa_qa' not in line for line in sinks.splitlines()):
            raise RuntimeError('Audio fixture contains an unexpected sink')
        self.remote = self.call('org.gnome.Mutter.RemoteDesktop',
                                '/org/gnome/Mutter/RemoteDesktop',
                                'org.gnome.Mutter.RemoteDesktop', 'CreateSession')[0]
        session_id = self.call('org.gnome.Mutter.RemoteDesktop', self.remote,
                               'org.freedesktop.DBus.Properties', 'Get', '(ss)',
                               ('org.gnome.Mutter.RemoteDesktop.Session', 'SessionId'))[0]
        cast = self.call('org.gnome.Mutter.ScreenCast', '/org/gnome/Mutter/ScreenCast',
                         'org.gnome.Mutter.ScreenCast', 'CreateSession', '(a{sv})',
                         ({'remote-desktop-session-id': GLib.Variant('s', session_id)},))[0]
        self.stream = self.call('org.gnome.Mutter.ScreenCast', cast,
                                'org.gnome.Mutter.ScreenCast.Session', 'RecordMonitor',
                                '(sa{sv})', ('Meta-0', {}))[0]
        self.input('Start')
        # A negotiated consumer is required for absolute input mapping.
        objects = json.loads(self.run(['pw-dump']))
        nodes = [n['id'] for n in objects if n['type'].endswith(':Node')
                 and n.get('info', {}).get('props', {}).get('media.class') == 'Stream/Output/Video']
        if len(nodes) != 1:
            raise RuntimeError('Expected one private compositor video stream')
        self.spawn('screen-consumer', ['gst-launch-1.0', '-q', 'pipewiresrc',
                                      f'path={nodes[0]}', 'always-copy=true', '!',
                                      'videoconvert', '!', 'fakesink', 'sync=false'])
        self.call('org.gnome.Shell', '/org/gnome/Shell', 'org.freedesktop.DBus.Properties',
                  'Set', '(ssv)', ('org.gnome.Shell', 'OverviewActive', GLib.Variant('b', False)))

    def chord(self, codes):
        for code in codes:
            self.input('NotifyKeyboardKeycode', '(ub)', (int(code), True))
        time.sleep(.02)
        for code in reversed(codes):
            self.input('NotifyKeyboardKeycode', '(ub)', (int(code), False))

    def text(self, value):
        letters = dict(zip('qwertyuiopasdfghjklzxcvbnm',
                           [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31, 32,
                            33, 34, 35, 36, 37, 38, 44, 45, 46, 47, 48, 49, 50]))
        keys = {**letters, **dict(zip('1234567890', range(2, 12))), ' ': 57, '\n': 28,
                '\t': 15, '-': 12, '=': 13, '[': 26, ']': 27, ';': 39, "'": 40,
                ',': 51, '.': 52, '/': 53, '`': 41, '\\': 43}
        shift = dict(zip('!@#$%^&*()_+{}:"<>?~|', '1234567890-=[];\',./`\\'))
        for char in value:
            normal = shift.get(char, char.lower())
            if normal not in keys:
                raise ValueError('Only US ASCII text is supported')
            self.chord(([42] if char.isupper() or char in shift else []) + [keys[normal]])

    def dispatch(self, request):
        action = request['action']
        if action == 'snapshot':
            path = (self.args.output / request.get('name', 'desktop.png')).resolve()
            if path.parent != self.args.output:
                raise ValueError('Screenshot must be inside the output directory')
            self.call('org.gnome.Shell.Screenshot', '/org/gnome/Shell/Screenshot',
                      'org.gnome.Shell.Screenshot', 'Screenshot', '(bbs)',
                      (bool(request.get('cursor')), False, str(path)))
            return {'path': str(path)}
        if action in ('move', 'click'):
            x, y = float(request['x']), float(request['y'])
            if not (0 <= x < 1280 and 0 <= y < 800):
                raise ValueError('Point is outside the virtual monitor')
            self.input('NotifyPointerMotionAbsolute', '(sdd)', (self.stream, x, y))
            if action == 'click':
                time.sleep(.1)
                for pressed in (True, False):
                    self.input('NotifyPointerButton', '(ib)', (272, pressed))
        elif action == 'key':
            self.chord(request.get('codes', [request.get('code', 28)]))
        elif action == 'text':
            self.text(request['text'])
        elif action == 'launch':
            if self.manager and self.manager.poll() is None:
                raise ValueError('Manager is already running')
            env = dict(os.environ, PIRWA_LIBVIRT_URI=self.args.uri, GTK_A11Y='none')
            self.manager = self.spawn('manager', [str(self.args.binary)], env)
            return {'pid': self.manager.pid}
        elif action == 'close-manager':
            if self.manager:
                self.manager.terminate()
                self.manager.wait(timeout=10)
        elif action == 'close-viewers':
            self.close_viewers()
        elif action == 'fixture':
            # qa-vm independently validates the opt-in and disposable identity.
            return {'result': self.run([str(self.args.binary.parent.parent / 'tests/qa-vm'),
                                       self.args.uri, *request['args']])}
        elif action == 'app-action':
            if request['name'] not in ('console', 'search', 'refresh', 'new'):
                raise ValueError('Use native UI or an identity-checked fixture command for mutations')
            self.call('io.github.stonega.Pirwa', '/io/github/stonega/Pirwa/window/1',
                      'org.gtk.Actions', 'Activate', '(sava{sv})', (request['name'], [], {}))
        elif action != 'exit':
            raise ValueError('Unknown controller action')
        return {}

    def close_viewers(self):
        # Viewer processes intentionally outlive the manager in normal use.
        # This fixture owns only viewers in its private desktop runtime.
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                if (proc / 'comm').read_text().strip() != 'virt-viewer':
                    continue
                env = dict(item.split('=', 1) for item in (proc / 'environ').read_text().split('\0')
                           if '=' in item)
                if env.get('XDG_RUNTIME_DIR') == os.environ['XDG_RUNTIME_DIR']:
                    os.kill(int(proc.name), signal.SIGTERM)
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue

    def close(self):
        self.close_viewers()
        if self.remote:
            try:
                self.input('Stop')
            except GLib.Error:
                pass
        for child in reversed(self.children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--uri')
    parser.add_argument('--request', help='Send one JSON request to an existing controller')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build-perf/src/pirwa')
    parser.add_argument('--viewer-dir', type=Path)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.request:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(150)
            client.connect(str(args.output / 'control.sock'))
            client.sendall((json.dumps(json.loads(args.request)) + '\n').encode())
            response = json.loads(client.makefile().readline())
            print(json.dumps(response))
            return 0 if response['ok'] else 1
    if not args.uri:
        parser.error('--uri is required when starting a controller')
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR', '/'))
    if os.environ.get('PIRWA_QA_ISOLATED') != '1' or not runtime.parent.name.startswith('pirwa-gnome-'):
        parser.error('Run only inside test-gnome.py')
    if os.environ.get('PIRWA_RUN_KVM_TESTS') != '1':
        parser.error('Set PIRWA_RUN_KVM_TESTS=1 for explicit real-guest QA')
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ.update(PIPEWIRE_RUNTIME_DIR=str(runtime), PIPEWIRE_REMOTE='pipewire-0',
                      PULSE_SERVER=f'unix:{runtime}/pulse/native', PULSE_SINK='pirwa_qa',
                      PULSE_SOURCE='pirwa_qa.monitor')
    if args.viewer_dir:
        os.environ['PATH'] = str(args.viewer_dir.resolve()) + ':' + os.environ['PATH']
    state = Path(os.environ['XDG_STATE_HOME']) / 'pirwa/boxes-import.ini'
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text('[Boxes]\nIntroductionSeen=true\nFlatpak=false\n')
    desktop = Desktop(args)
    endpoint = args.output / 'control.sock'
    # Refuse an existing endpoint instead of unlinking an active session.
    server = socket.socket(socket.AF_UNIX)
    bound = False

    def interrupted(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        desktop.start()
        server.bind(str(endpoint))
        bound = True
        os.chmod(endpoint, 0o600)
        server.listen()
        (args.output / 'ready.json').write_text(json.dumps({'pid': os.getpid(), 'runtime': str(runtime)}))
        while True:
            with server.accept()[0] as peer:
                peer.settimeout(120)
                request = json.loads(peer.makefile('r').readline(65536))
                try:
                    reply = {'ok': True, **desktop.dispatch(request)}
                except Exception as error:
                    reply = {'ok': False, 'error': str(error)}
                peer.sendall((json.dumps(reply) + '\n').encode())
                if request['action'] == 'exit':
                    break
    finally:
        server.close()
        if bound:
            endpoint.unlink(missing_ok=True)
        desktop.close()


if __name__ == '__main__':
    raise SystemExit(main())
