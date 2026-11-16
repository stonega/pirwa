#!/usr/bin/env python3
"""Opt-in Linux guest/storage recovery in an isolated libvirt session."""

import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import platform
import select
import selectors
import shutil
import signal
import stat
import struct
import subprocess
import tempfile
import time
import tty
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('daemon_qa', ROOT / 'scripts/test-libvirt-daemon.py')
daemon_qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daemon_qa)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def initramfs(binary):
    """Small deterministic newc archive; no root, device nodes or host initrd."""
    archive = bytearray()
    for ino, (name, mode, data) in enumerate([
            ('dev', stat.S_IFDIR | 0o755, b''), ('mnt', stat.S_IFDIR | 0o755, b''),
            ('init', stat.S_IFREG | 0o755, binary), ('TRAILER!!!', 0, b'')], 1):
        encoded = name.encode() + b'\0'
        fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
        archive.extend(('070701' + ''.join(f'{field:08x}' for field in fields)).encode())
        archive.extend(encoded)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))
    return gzip.compress(bytes(archive), mtime=0)


class Guest:
    def __init__(self, base, env, output, kernel, disk_format):
        self.base, self.env, self.output = base, env, output
        self.disk_format = disk_format
        self.uuid = str(uuid.uuid4())
        self.name = 'pirwa-qa-' + self.uuid
        self.disk = base / ('data/heartbeat.' + disk_format)
        self.serial = None
        self.pidfd = None
        self.buffer = b''
        self.counter = 0
        self.clean = None
        self.boots = []
        self.identity = None
        self.selector = selectors.DefaultSelector()
        self.console_log = (output / 'guest.serial.log').open('wb')
        self.command_log = (output / 'virsh.log').open('w')
        self.report = {'uuid': self.uuid, 'name': self.name, 'memory_mib': 512,
                       'disk_format': disk_format, 'disk_bus': 'virtio', 'filesystem': 'ext4',
                       'checkpoints': [], 'disk_checks': [], 'boots': self.boots}
        binary = output / 'guest-init'
        command = ['gcc', '-std=c17', '-Os', '-Wall', '-Wextra', '-Werror', '-static',
                   '-nostdlib', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                   '-fno-pie', '-no-pie', '-Wl,-e,_start',
                   str(ROOT / 'tests/qa-guest-init.c'), str(ROOT / 'tests/qa-guest-init-start.S'),
                   '-o', str(binary)]
        result = subprocess.run(command, check=False, env=env, capture_output=True, text=True, timeout=30)
        (output / 'guest-build.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        ramdisk = base / 'data/initramfs.cpio.gz'
        ramdisk.write_bytes(initramfs(binary.read_bytes()))
        shutil.copy2(ramdisk, output / ramdisk.name)
        private_kernel = base / 'data/vmlinuz'
        shutil.copyfile(kernel, private_kernel)
        self.report.update(kernel=str(kernel), kernel_sha256=sha(kernel),
                           init_sha256=sha(binary), initramfs_sha256=sha(ramdisk),
                           compiler_command=command)
        raw = base / 'data/heartbeat.raw'
        with raw.open('wb') as disk:
            disk.truncate(64 * 1024 * 1024)
        subprocess.run(['mkfs.ext4', '-q', '-F', str(raw)], env=env,
                       check=True, capture_output=True, timeout=30)
        if disk_format != 'raw':
            subprocess.run(['qemu-img', 'convert', '-f', 'raw', '-O', disk_format,
                            str(raw), str(self.disk)], env=env, check=True,
                           capture_output=True, timeout=30)
            raw.unlink()
        domain = ET.Element('domain', type='kvm')
        ET.SubElement(domain, 'name').text = self.name
        ET.SubElement(domain, 'uuid').text = self.uuid
        ET.SubElement(domain, 'memory', unit='MiB').text = '512'
        ET.SubElement(domain, 'vcpu').text = '1'
        os_xml = ET.SubElement(domain, 'os')
        ET.SubElement(os_xml, 'type', arch='x86_64', machine='q35').text = 'hvm'
        ET.SubElement(os_xml, 'kernel').text = str(private_kernel)
        ET.SubElement(os_xml, 'initrd').text = str(ramdisk)
        ET.SubElement(os_xml, 'cmdline').text = 'console=ttyS0,115200 rdinit=/init panic=-1 quiet'
        ET.SubElement(domain, 'cpu', mode='host-passthrough')
        features = ET.SubElement(domain, 'features')
        ET.SubElement(features, 'acpi')
        ET.SubElement(features, 'apic')
        for event in ['on_poweroff', 'on_reboot', 'on_crash']:
            ET.SubElement(domain, event).text = 'destroy'
        devices = ET.SubElement(domain, 'devices')
        ET.SubElement(devices, 'emulator').text = shutil.which('qemu-system-x86_64')
        disk = ET.SubElement(devices, 'disk', type='file', device='disk')
        ET.SubElement(disk, 'driver', name='qemu', type=disk_format, cache='writethrough')
        ET.SubElement(disk, 'source', file=str(self.disk))
        ET.SubElement(disk, 'target', dev='vda', bus='virtio')
        serial = ET.SubElement(devices, 'serial', type='pty')
        ET.SubElement(serial, 'target', port='0')
        console = ET.SubElement(devices, 'console', type='pty')
        ET.SubElement(console, 'target', type='serial', port='0')
        ET.SubElement(devices, 'memballoon', model='none')
        self.xml = base / 'data/guest.xml'
        ET.ElementTree(domain).write(self.xml, encoding='unicode')
        shutil.copy2(self.xml, output / 'guest.xml')

    def virsh(self, *arguments, check=True):
        # A second client operates only this generated UUID in the private XDG
        # session. Pirwa itself continues to use its libvirt worker/C API.
        result = subprocess.run(['virsh', '--connect', 'qemu:///session', *arguments],
                                check=False, env=self.env, capture_output=True, timeout=20)
        self.command_log.write(json.dumps({'arguments': arguments, 'returncode': result.returncode,
                                          'stderr': result.stderr.decode(errors='replace')}) + '\n')
        self.command_log.flush()
        if check:
            result.check_returncode()
        return result

    def attach(self):
        self.detach()
        document = ET.fromstring(self.virsh('dumpxml', self.uuid).stdout)
        path = document.find('./devices/serial/source').get('path')
        if not path.startswith('/dev/pts/'):
            raise RuntimeError('Expected the generated guest serial PTY')
        self.serial = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        tty.setraw(self.serial)
        self.selector.register(self.serial, selectors.EVENT_READ)
        candidates = list((self.base / 'runtime').rglob(self.name + '.pid'))
        if len(candidates) != 1:
            raise RuntimeError(f'Expected one private guest PID file, found {candidates}')
        self.pid = int(candidates[0].read_text())
        self.pidfd = os.pidfd_open(self.pid)
        self.identity = self.process_identity()
        self.buffer = b''
        self.clean = None

    def process_identity(self):
        proc = Path('/proc') / str(self.pid)
        arguments = (proc / 'cmdline').read_bytes().split(b'\0')
        if (b'-uuid' not in arguments or arguments[arguments.index(b'-uuid') + 1] != self.uuid.encode()
                or not (proc / 'exe').resolve().name.startswith('qemu-system-')):
            raise RuntimeError('Guest process does not match the generated UUID')
        start = (proc / 'stat').read_text().rsplit(') ', 1)[1].split()[19]
        return {'pid': self.pid, 'start_ticks': start}

    def pump(self, timeout):
        for key, _ in self.selector.select(timeout):
            data = os.read(key.fd, 65536)
            if not data:
                raise RuntimeError('Guest serial closed before the expected checkpoint')
            self.console_log.write(data)
            self.console_log.flush()
            self.buffer += data
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                line = line.strip().decode(errors='replace')
                if line.startswith('PIRWA_ERROR'):
                    raise RuntimeError(line)
                if line.startswith('PIRWA_BOOT '):
                    self.boots.append(int(line.split()[-1]))
                if line.startswith('PIRWA_HEARTBEAT '):
                    value = int(line.split()[-1])
                    if value <= self.counter:
                        raise RuntimeError('Durable heartbeat did not advance monotonically')
                    self.counter = value
                if line.startswith('PIRWA_CLEAN '):
                    self.clean = int(line.split()[-1])

    def progress(self, after, label):
        deadline = time.monotonic() + 30
        while self.counter <= after:
            if time.monotonic() >= deadline:
                raise RuntimeError('Guest heartbeat timed out: ' + label)
            self.pump(.25)
        identity = self.process_identity()
        if identity != self.identity:
            raise RuntimeError('Guest process changed during daemon recovery')
        sample = {'label': label, 'counter': self.counter, 'boots': list(self.boots),
                  'process': identity, 'monotonic': time.monotonic()}
        self.report['checkpoints'].append(sample)
        return self.counter

    def finish(self, client):
        since = len(client.events)
        os.write(self.serial, b'Q\n')
        deadline = time.monotonic() + 20
        while self.clean is None:
            if time.monotonic() >= deadline:
                raise RuntimeError('Guest did not unmount and shut down cleanly')
            self.pump(.25)
        client.receive(lambda event: state(event, self.uuid, 5), since=since)
        exited = select.poll()
        exited.register(self.pidfd, select.POLLIN)
        if not exited.poll(5000):
            raise RuntimeError('Guest process did not exit before offline disk inspection')
        image_check = None
        inspection = self.disk
        if self.disk_format == 'qcow2':
            result = subprocess.run(['qemu-img', 'check', '-f', 'qcow2', '--output=json', str(self.disk)],
                                    env=self.env, check=True, capture_output=True, text=True, timeout=20)
            image_check = json.loads(result.stdout)
            inspection = self.base / 'data/inspection.raw'
            subprocess.run(['qemu-img', 'convert', '-f', 'qcow2', '-O', 'raw',
                            str(self.disk), str(inspection)], env=self.env, check=True,
                           capture_output=True, timeout=20)
        result = subprocess.run(['e2fsck', '-f', '-n', str(inspection)], check=False, env=self.env,
                                capture_output=True, text=True, timeout=20)
        if result.returncode != 0:
            raise RuntimeError('Guest ext4 failed its offline integrity check: ' + result.stdout + result.stderr)
        contents = subprocess.run(['debugfs', '-R', 'cat /heartbeat', str(inspection)], env=self.env,
                                  check=True, capture_output=True, timeout=20).stdout
        if len(contents) != 24:
            raise RuntimeError('Unexpected durable record length')
        magic, counter, inverse = struct.unpack('<8sQQ', contents)
        if magic != b'PIRWA01\n' or counter != self.clean or inverse != (~counter & ((1 << 64) - 1)):
            raise RuntimeError('Offline disk record differs from the guest synced record')
        self.report['disk_checks'].append({'counter': counter, 'inverse': inverse,
                                           'e2fsck_returncode': result.returncode,
                                           'e2fsck_output': result.stdout + result.stderr,
                                           'image_check': image_check,
                                           'disk_sha256': sha(self.disk)})
        if inspection != self.disk:
            inspection.unlink()
        return counter

    def detach(self):
        if self.serial is not None:
            self.selector.unregister(self.serial)
            os.close(self.serial)
            self.serial = None
        if self.pidfd is not None:
            os.close(self.pidfd)
            self.pidfd = None

    def cleanup(self, daemon):
        try:
            if daemon.process is None:
                daemon.start()
            self.virsh('destroy', self.uuid, check=False)
            self.virsh('undefine', self.uuid, check=False)
        finally:
            try:
                self.report['forced_process_cleanup'] = False
                if self.pidfd is not None:
                    poll = select.poll()
                    poll.register(self.pidfd, select.POLLIN)
                    for sig in (signal.SIGTERM, signal.SIGKILL):
                        if poll.poll(0):
                            break
                        # pidfd binds this signal to the exact validated QEMU
                        # process; never scan/kill by a process or domain name.
                        try:
                            signal.pidfd_send_signal(self.pidfd, sig)
                            self.report['forced_process_cleanup'] = True
                        except ProcessLookupError:
                            pass
                        poll.poll(5000)
                    self.report['qemu_exit_confirmed'] = bool(poll.poll(0))
                    if not self.report['qemu_exit_confirmed']:
                        raise RuntimeError('Private QEMU did not exit during cleanup')
            finally:
                self.detach()
                self.selector.close()
                self.console_log.close()
                self.command_log.close()


def state(event, identity, expected):
    return (event.get('ok') and event.get('machines') == 1
            and event.get('domains') == [{'uuid': identity, 'state': expected}])


def exercise(client, daemon, guest, report):
    def passed(label):
        report['checks'].append(label)
        print('PASS ' + label, flush=True)

    def running(event, metadata=False):
        if (not state(event, guest.uuid, 1)
                or (metadata and (not event.get('host') or not event.get('pools')))):
            raise RuntimeError('Recovered inventory lost running guest or metadata: ' + str(event))

    client.receive(lambda event: event['event'] == 'ready')
    daemon_qa.healthy(client.request('refresh'))
    daemon.activated()
    guest.virsh('define', str(guest.xml))
    running(client.request('start ' + guest.uuid))
    guest.attach()
    guest.progress(2, 'initial-linux-virtio-ext4')
    if guest.boots != [0]:
        raise RuntimeError('Guest did not boot with an empty heartbeat file')
    passed('linux-guest-synced-ext4-heartbeat')

    for crash, label in [(False, 'graceful'), (True, 'crash')]:
        before = guest.progress(guest.counter, label + '-before-daemon-exit')
        since = len(client.events)
        daemon.stop(crash=crash)
        client.receive(lambda event: event['event'] == 'error', since)
        during = guest.progress(before + 2, label + '-while-daemon-absent')
        since = len(client.events)
        daemon.start()
        recovered = client.receive(lambda event: event['event'] == 'inventory', since)
        running(recovered, metadata=True)
        daemon.activated()
        guest.progress(during + 2, label + '-after-reconnect')
        if guest.boots != [0]:
            raise RuntimeError('Guest rebooted while replacing its daemon')
        passed(label + '-daemon-restart-preserves-running-guest-and-writes')

    paused = client.request('pause ' + guest.uuid)
    if not state(paused, guest.uuid, 3):
        raise RuntimeError('Recovered backend could not pause its guest')
    running(client.request('resume ' + guest.uuid))
    guest.progress(guest.counter + 2, 'after-backend-pause-resume')
    passed('recovered-backend-pause-resume')
    saved = guest.finish(client)
    passed('clean-unmount-and-offline-ext4-integrity')
    running(client.request('start ' + guest.uuid))
    guest.attach()
    guest.progress(saved + 2, 'reboot-with-durable-record')
    if guest.boots != [0, saved]:
        raise RuntimeError('Guest did not recover the synced disk record on reboot')
    guest.finish(client)
    passed('guest-reboot-preserves-record-and-clean-filesystem')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/qa/guest-recovery')
    parser.add_argument('--kernel', type=Path, default=Path('/boot/vmlinuz-' + platform.release()))
    parser.add_argument('--disk-format', choices=['qcow2', 'raw'], default='qcow2')
    args = parser.parse_args()
    if (os.geteuid() == 0 or os.environ.get('PIRWA_RUN_DAEMON_TESTS') != '1'
            or os.environ.get('PIRWA_RUN_KVM_TESTS') != '1'):
        parser.error('Set PIRWA_RUN_DAEMON_TESTS=1 and PIRWA_RUN_KVM_TESTS=1 as a regular user')
    if platform.machine() != 'x86_64' or not os.access('/dev/kvm', os.R_OK | os.W_OK):
        parser.error('An x86_64 host with accessible KVM is required')
    for command in ['virtqemud', 'systemd-socket-activate', 'qemu-system-x86_64', 'virsh',
                    'gcc', 'qemu-img', 'mkfs.ext4', 'e2fsck', 'debugfs']:
        if not shutil.which(command):
            parser.error('Required QA command is missing: ' + command)
    config = args.kernel.with_name(args.kernel.name.replace('vmlinuz-', 'config-', 1))
    if not args.kernel.is_file() or not config.is_file():
        parser.error('Use --kernel /boot/vmlinuz-VERSION with its sibling config-VERSION')
    required = ['BLK_DEV_INITRD', 'RD_GZIP', 'PCI', 'DEVTMPFS', 'VIRTIO_BLK',
                'VIRTIO_PCI', 'EXT4_FS', 'SERIAL_8250_CONSOLE']
    configuration = config.read_text().splitlines()
    if any('CONFIG_' + key + '=y' not in configuration for key in required):
        parser.error('The QA kernel needs built-in initrd/gzip/PCI/devtmpfs/virtio/ext4/serial support')
    memory = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                      if line.startswith('MemAvailable:')))
    if memory < (512 + 1024) * 1024:
        parser.error('Need 512 MiB guest memory plus 1024 MiB available host headroom')
    binary = (args.build / 'tests/test-backend-daemon').resolve()
    if not binary.is_file():
        parser.error('Build test-backend-daemon first')
    if not (args.build / 'data/gschemas.compiled').is_file():
        parser.error('Run meson compile -C BUILD to build matching GSettings schemas')
    signal.signal(signal.SIGTERM, daemon_qa.interrupted)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    pinned, schemas = daemon_qa.pin_client(args.build, args.output)
    sources = [ROOT / p for p in ['scripts/test-guest-recovery.py', 'scripts/test-libvirt-daemon.py',
                                  'tests/qa-guest-init.c', 'tests/qa-guest-init-start.S']]
    sources += [p for folder in ['src', 'tests'] for p in (ROOT / folder).glob('*.[ch]')]
    report = {'passed': False, 'checks': [], 'binary_sha256': sha(pinned),
              'schema_sha256': sha(schemas / 'gschemas.compiled'),
              'scope': 'Disposable direct-kernel Linux/KVM guest, private socket-activated session, virtio disk and ext4; no existing host resources',
              'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in sources},
              'mem_available_kib_at_start': memory, 'kernel_config_sha256': sha(config),
              'virtqemud_version': subprocess.check_output(['virtqemud', '--version'], text=True).strip()}
    with tempfile.TemporaryDirectory(prefix='pirwa-libvirt-', dir='/tmp') as directory:
        base = Path(directory)
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LIBVIRT_AUTOSTART': '0'}
        for key, suffix in [('RUNTIME_DIR', 'runtime'), ('CONFIG_HOME', 'config'),
                            ('DATA_HOME', 'data'), ('CACHE_HOME', 'cache'), ('STATE_HOME', 'state')]:
            path = base / suffix
            path.mkdir(mode=0o700)
            env['XDG_' + key] = str(path)
        daemon = daemon_qa.Daemon(base, env, args.output, activation=True)
        client_env = dict(env, PIRWA_RUN_DAEMON_TESTS='1', PIRWA_RUN_KVM_TESTS='1',
                          PIRWA_DAEMON_TEST_ROOT=str(base), PIRWA_DAEMON_TEST_SOCKET_ACTIVATION='1')
        client_env.update(GSETTINGS_SCHEMA_DIR=str(schemas), GSETTINGS_BACKEND='memory')
        for key in ['LD_LIBRARY_PATH', 'ASAN_OPTIONS', 'UBSAN_OPTIONS', 'LSAN_OPTIONS', 'DEBUGINFOD_URLS']:
            if key in os.environ:
                client_env[key] = os.environ[key]
        client = guest = None
        try:
            guest = Guest(base, env, args.output, args.kernel, args.disk_format)
            daemon.start()
            daemon.waiting_for_connection()
            client = daemon_qa.Client(pinned, client_env, args.output)
            exercise(client, daemon, guest, report)
            report['passed'] = True
        except (Exception, KeyboardInterrupt) as error:  # noqa: BLE001 - fixture cleanup always runs
            report['error'] = str(error) or type(error).__name__
        finally:
            if client:
                client.close()
                report['client_exitcode'] = client.process.returncode
                report['events'] = client.events
            if guest:
                try:
                    guest.cleanup(daemon)
                except Exception as error:  # noqa: BLE001 - still stop daemon/helpers
                    report['cleanup_error'] = str(error)
                    report['passed'] = False
                report['guest'] = guest.report
            daemon.stop()
            report['daemon_generations'] = daemon.generations
            report['process_cleanup'] = daemon_qa.reap_helpers(base / 'runtime')
            report['private_domain_files'] = [str(p.relative_to(base)) for p in base.glob('config/libvirt/qemu/*.xml')]
            report['private_storage_files'] = [str(p.relative_to(base)) for p in base.glob('config/libvirt/storage/*.xml')]
            if (report.get('client_exitcode') != 0 or report['process_cleanup']['remaining']
                    or report['private_domain_files'] or report['private_storage_files']):
                report['passed'] = False
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    report['private_root_removed'] = not base.exists()
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{'PASS' if report['passed'] else 'FAIL'} guest recovery: {args.output}")
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
