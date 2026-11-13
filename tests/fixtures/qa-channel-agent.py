#!/usr/bin/env python3
"""Run manually as root INSIDE a disposable guest with the QA virtio channel.

Test-only argv/JSON command bridge; no network listener and no product feature.
"""
import json
import os
from pathlib import Path
import stat
import subprocess
import time


def main():
    if os.environ.get('PIRWA_RUN_KVM_TESTS') != '1':
        raise SystemExit('Set PIRWA_RUN_KVM_TESTS=1 inside the disposable guest')
    port = Path('/dev/virtio-ports/org.pirwa.qa.0')
    if not stat.S_ISCHR(port.stat().st_mode):
        raise SystemExit('Expected the dedicated QA character device')
    with port.open('r+b', buffering=0) as channel:
        while True:
            line = channel.readline(65536)
            if not line:
                time.sleep(.1)
                continue
            try:
                argv = json.loads(line)
                if not isinstance(argv, list) or not argv or not all(isinstance(v, str) for v in argv):
                    raise ValueError('Expected a nonempty argv array')
                result = subprocess.run(argv, capture_output=True, text=True, timeout=120)
                response = dict(code=result.returncode, out=result.stdout, err=result.stderr)
            except Exception as error:
                response = dict(code=1, out='', err=str(error))
            try:
                channel.write((json.dumps(response) + '\n').encode())
            except OSError:
                # A timed-out host command may disconnect before its response.
                time.sleep(.1)


if __name__ == '__main__':
    main()
