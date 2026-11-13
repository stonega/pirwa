#!/usr/bin/env python3
"""Paired workload sampling in an explicitly prepared disposable guest.

The channel agent accepts JSON argv arrays and returns code/out/err JSON.
Prepare and mount a disposable disk at /mnt/pirwa-qa before running this tool.
This measures manager presence within one boot, separately from start latency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
WORKLOAD = r'''
import hashlib,json,mmap,os,sys,time
root='/mnt/pirwa-qa'
assert os.path.ismount(root) and os.path.isfile(root+'/.pirwa-qa-disposable')
block=1024*1024
count=1024
passes=int(sys.argv[1])
iterations=int(sys.argv[2])
buf=mmap.mmap(-1,block)
buf[:]=hashlib.shake_256(b'Pirwa QA fixed seed').digest(block)
fd=os.open(root+'/workload.bin',os.O_CREAT|os.O_RDWR|os.O_DIRECT,0o600)
os.ftruncate(fd,count*block)
def write():
 os.lseek(fd,0,0)
 for _ in range(count):assert os.write(fd,buf)==block
 os.fsync(fd)
def read():
 os.lseek(fd,0,0)
 for _ in range(count):assert os.readv(fd,[buf])==block
write() # identical preconditioning for each sample, outside the timing span
t=time.monotonic();write();write_seconds=time.monotonic()-t
t=time.monotonic()
for _ in range(passes):read()
read_seconds=time.monotonic()-t
t=time.monotonic();digest=hashlib.pbkdf2_hmac('sha256',b'Pirwa QA CPU',b'fixed salt',iterations).hex();cpu_seconds=time.monotonic()-t
os.close(fd);buf.close()
print(json.dumps(dict(cpu_seconds=cpu_seconds,cpu_digest=digest,write_mib_s=count/write_seconds,read_mib_s=count*passes/read_seconds,write_seconds=write_seconds,read_seconds=read_seconds,write_bytes=count*block,read_bytes=count*block*passes,guest_direct_io=True)))
'''


def controller(directory, action):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(30)
        client.connect(str(directory / 'control.sock'))
        client.sendall((json.dumps({'action': action}) + '\n').encode())
        result = json.loads(client.makefile().readline())
        if not result['ok']:
            raise RuntimeError(result)
        return result


def workload(channel, passes, iterations):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(120)
        client.connect(str(channel))
        client.sendall((json.dumps(['python3', '-c', WORKLOAD, str(passes), str(iterations)]) + '\n').encode())
        result = json.loads(client.makefile().readline())
        if result['code']:
            raise RuntimeError(result['err'])
        return json.loads(result['out'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', type=Path, required=True)
    parser.add_argument('--controller', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=12)
    parser.add_argument('--read-passes', type=int, default=8)
    parser.add_argument('--cpu-iterations', type=int, default=24000000)
    parser.add_argument('--settle-seconds', type=int, default=5)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get('PIRWA_RUN_KVM_TESTS') != '1' or not 3 <= args.pairs <= 30:
        parser.error('Set PIRWA_RUN_KVM_TESTS=1 and use 3–30 pairs')
    if not (1 <= args.read_passes <= 16 and 1000000 <= args.cpu_iterations <= 30000000
            and 2 <= args.settle_seconds <= 30):
        parser.error('Use 1–16 read passes, 1–30 million CPU iterations, and 2–30 settling seconds')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                                      cwd=ROOT, text=True).strip(),
              'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
              'scope': 'Alternating manager-absent/manager-idle samples in the same running VM; warm host caches, guest O_DIRECT. Separate from start latency and cold boot.',
              'workload_sha256': hashlib.sha256(WORKLOAD.encode()).hexdigest(),
              'parameters': {'read_passes': args.read_passes, 'cpu_iterations': args.cpu_iterations,
                             'settle_seconds': args.settle_seconds, 'pairs': args.pairs},
              'host_load_at_start': os.getloadavg(), 'samples': []}

    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n')

    try:
        controller(args.controller, 'close-manager')
        report['warmup'] = workload(args.channel, args.read_passes, args.cpu_iterations)
        save()
        for pair in range(args.pairs):
            for present in ([False, True] if pair % 2 == 0 else [True, False]):
                controller(args.controller, 'close-manager')
                manager_pid = None
                if present:
                    manager_pid = controller(args.controller, 'launch')['pid']
                # Fixed settling period in both conditions, not an assertion.
                time.sleep(args.settle_seconds)
                if manager_pid and not Path(f'/proc/{manager_pid}').exists():
                    raise RuntimeError('Manager exited before the workload')
                sample = workload(args.channel, args.read_passes, args.cpu_iterations)
                if sample['cpu_digest'] != report['warmup']['cpu_digest']:
                    raise RuntimeError('Guest workload digest changed')
                if manager_pid and not Path(f'/proc/{manager_pid}').exists():
                    raise RuntimeError('Manager exited during the workload')
                sample.update(pair=pair, manager_present=present, host_load=os.getloadavg())
                report['samples'].append(sample)
                save()
                print(f"pair {pair + 1}/{args.pairs}: {'manager' if present else 'baseline'}", flush=True)
        ratios = {}
        for key in ('cpu_seconds', 'write_mib_s', 'read_mib_s'):
            pairs = []
            for pair in range(args.pairs):
                values = {s['manager_present']: s[key] for s in report['samples'] if s['pair'] == pair}
                pairs.append(values[True] / values[False])
            ratios[key] = {'paired_ratios': pairs, 'median_ratio': statistics.median(pairs),
                           'minimum_ratio': min(pairs), 'maximum_ratio': max(pairs)}
        report.update(ratios=ratios, host_load_at_end=os.getloadavg(), completed=True)
        save()
    finally:
        controller(args.controller, 'close-manager')


if __name__ == '__main__':
    main()
