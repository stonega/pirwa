#!/usr/bin/env python3
"""Build/test a frozen source snapshot against Fedora RPM dependencies in rootless Podman."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/qa/clean-build')
    parser.add_argument('--image', default='localhost/pirwa-qa:fedora45')
    parser.add_argument('--build-image', action='store_true')
    parser.add_argument('--build-network', choices=['private', 'host'], default='private',
                        help='Use host only when the build needs a localhost package proxy')
    parser.add_argument('--rpm', action='store_true', help='Also build/install the development RPM')
    args = parser.parse_args()
    if os.geteuid() == 0 or not shutil.which('podman'):
        parser.error('Use installed rootless Podman as your regular user')
    if run(['podman', 'info', '--format', '{{.Host.Security.Rootless}}'],
           capture_output=True, text=True).stdout.strip() != 'true':
        parser.error('Rootless Podman is required')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.build_image:
        # There is no COPY instruction: use an empty context so local build
        # directories, ignored files and credentials never enter that context.
        with (tempfile.TemporaryDirectory(prefix='pirwa-container-context-') as context,
              (args.output / 'image-build.log').open('w') as log):
            run(['podman', 'build', '--network=' + args.build_network, '--memory=2g',
                 '--tag', args.image, '--file', str(ROOT / 'packaging/Containerfile.qa'), context],
                stdout=log, stderr=subprocess.STDOUT)
    image = json.loads(run(['podman', 'image', 'inspect', args.image],
                           capture_output=True, text=True).stdout)[0]
    snapshot = args.output / 'source'
    snapshot.mkdir()
    files = run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
                cwd=ROOT, capture_output=True).stdout.decode().split('\0')
    sources = {}
    for name in sorted(set(files) - {''}):
        source = ROOT / name
        if not source.exists() and not source.is_symlink():
            continue  # A staged/tracked deletion is absent from this snapshot.
        target = snapshot / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            link = os.readlink(source)
            target.symlink_to(link)
            sources[name] = {'symlink': link}
        else:
            data = source.read_bytes()
            target.write_bytes(data)
            target.chmod(source.stat().st_mode & 0o777)
            sources[name] = {'sha256': hashlib.sha256(data).hexdigest()}
    results = args.output / 'results'
    results.mkdir()
    report = {'passed': False, 'rootless': True, 'network_during_checks': 'none',
              'source_commit': run(['git', 'rev-parse', 'HEAD'], cwd=ROOT,
                                   capture_output=True, text=True).stdout.strip(),
              'source_dirty': bool(run(['git', 'status', '--porcelain'], cwd=ROOT,
                                       capture_output=True, text=True).stdout.strip()),
              'source_manifest': sources, 'image_id': image['Id'],
              'image_digest': image.get('Digest'), 'rpm_requested': args.rpm}
    command = ['podman', 'run', '--rm', '--pull=never', '--network=none',
               '--http-proxy=false', '--memory=3g', '--cpus=2',
               '--volume', str(snapshot) + ':/src:ro,Z',
               '--volume', str(results) + ':/out:Z', image['Id'],
               '/usr/bin/env', '-i', 'PATH=/usr/bin:/bin:/usr/sbin:/sbin', 'LANG=C.UTF-8',
               '/bin/sh', '/src/scripts/check-native-build.sh']
    if args.rpm:
        command.append('--rpm')
    with (args.output / 'checks.log').open('w') as log:
        completed = subprocess.run(command, check=False, stdout=log, stderr=subprocess.STDOUT)
    report['returncode'] = completed.returncode
    report['last_stage'] = (results / 'stage').read_text().strip() if (results / 'stage').exists() else 'container-start'
    report['headless'] = {}
    for build in ['normal', 'sanitizer']:
        path = results / build / 'testlog.json'
        if path.exists():
            rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
            report['headless'][build] = [{k:row.get(k) for k in ['name', 'result', 'returncode', 'duration']}
                                         for row in rows]
    report['source_drift_since_snapshot'] = [name for name, item in sources.items()
                                            if 'sha256' in item and (not (ROOT / name).exists()
                                            or hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != item['sha256'])]
    report['passed'] = completed.returncode == 0 and report['last_stage'] == 'complete'
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{'PASS' if report['passed'] else 'FAIL'} clean native build: {args.output}")
    if not report['passed']:
        print('Last stage:', report['last_stage'])
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
