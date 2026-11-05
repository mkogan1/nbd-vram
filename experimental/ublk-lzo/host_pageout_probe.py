"""Bounded host pageout experiment; caller owns and retains the ublk target."""
import os
from pathlib import Path
import select
import re
import errno
import signal
import subprocess
import time

DEVICE = '/dev/ublkb31'
CGROUP_ROOT = Path('/sys/fs/cgroup')


def available_bytes():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) * 1024
    raise RuntimeError('MemAvailable unavailable')


def major_faults(group):
    values = dict(line.split() for line in (group / 'memory.stat').read_text().splitlines())
    return int(values['pgmajfault'])


def device_read_bytes():
    fields = Path('/sys/class/block/ublkb31/stat').read_text().split()
    return int(fields[2]) * 512


def run_pageout(target, evidence, wait_log):
    """No global pressure, no changes to existing swap devices or zswap policy."""
    if available_bytes() < 4 * 1024**3:
        raise RuntimeError('pageout probe requires at least 4 GiB available RAM')
    root = CGROUP_ROOT
    if 'memory' not in (root / 'cgroup.subtree_control').read_text().split():
        raise RuntimeError('root memory controller must already be enabled')
    helper = Path(__file__).with_name('test-ublk-host-pageout')
    if not helper.is_file():
        raise RuntimeError('pageout helper must be built before activating swap')
    worker = None
    group = None
    active = False
    captured = bytearray()
    try:
        subprocess.run([str(helper), '--device', DEVICE, '--target-pid',
                        str(target.pid), '--check-device-only'], check=True)
        subprocess.run(['/usr/bin/mkswap', DEVICE], check=True)
        # High priority is temporary; the test itself requests only 16 MiB pageout.
        subprocess.run(['/usr/bin/swapon', '--priority', '32767', DEVICE], check=True)
        active = True
        # The shell waits without allocating the test mapping; exec preserves PID.
        worker = subprocess.Popen(['/bin/sh', '-c',
            'read -r start || exit 1; exec "$@"', 'pageout', str(helper),
            '--device', DEVICE, '--target-pid', str(target.pid)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={'PATH': '/usr/bin:/bin', 'LANG': 'C'}, bufsize=0)
        group = root / f'vram-pageout-test-{worker.pid}'
        group.mkdir()
        for name, value in [('memory.max', 64 * 1024**2),
                            ('memory.swap.max', 32 * 1024**2),
                            ('memory.zswap.max', 0), ('memory.zswap.writeback', 1)]:
            (group / name).write_text(str(value))
            if (group / name).read_text().strip() != str(value):
                raise RuntimeError('cgroup setting readback mismatch: ' + name)
        (group / 'cgroup.procs').write_text(str(worker.pid))
        worker.stdin.write(b'GO\n')
        worker.stdin.flush()
        deadline = time.monotonic() + 20
        ready = None
        while ready is None:
            if time.monotonic() >= deadline or worker.poll() is not None:
                raise RuntimeError('pageout helper failed before ready: ' + captured.decode(errors='replace'))
            if select.select([worker.stdout], [], [], 0.2)[0]:
                data = os.read(worker.stdout.fileno(), 4096)
                if not data or len(captured) + len(data) > 65536:
                    raise RuntimeError('pageout helper output ended or exceeded bound: ' + captured.decode(errors='replace'))
                captured.extend(data)
                ready = re.search(rb'^PAGEOUT_READY pages=(\d+)(?: type=\d+)? attributed=(\d+)\n', captured, re.MULTILINE)
        if not (1 <= int(ready[1]) <= 4096 and ready[1] == ready[2]):
            raise RuntimeError('pageout attribution count invalid')
        if available_bytes() < 2 * 1024**3:
            raise RuntimeError('available RAM fell below safety headroom')
        faults_before = major_faults(group)
        target.send_signal(signal.SIGUSR2)
        wait_log('UBLK_DRAIN_OK ')
        try:
            (group / 'memory.reclaim').write_text(str(16 * 1024**2))
        except OSError as error:
            if error.errno != errno.EAGAIN:
                raise
        reads_before = device_read_bytes()
        worker.stdin.write(b'R')
        worker.stdin.close()
        deadline = time.monotonic() + 45
        while worker.poll() is None or select.select([worker.stdout], [], [], 0)[0]:
            if time.monotonic() >= deadline:
                raise RuntimeError('pageout readback timed out')
            if select.select([worker.stdout], [], [], 0.2)[0]:
                data = os.read(worker.stdout.fileno(), 4096)
                if not data:
                    break
                captured.extend(data)
                if len(captured) > 65536:
                    raise RuntimeError('pageout output exceeded bound')
        if worker.wait(timeout=5) or b'PAGEOUT_READBACK_PASS' not in captured:
            raise RuntimeError('pageout readback failed: ' + captured.decode(errors='replace'))
        fault_delta = major_faults(group) - faults_before
        if fault_delta <= 0:
            raise RuntimeError('readback did not prove major page faults in the private cgroup')
        captured.extend(f'PAGEOUT_MAJOR_FAULTS delta={fault_delta}\n'.encode())
        read_delta = device_read_bytes() - reads_before
        if read_delta < int(ready[1]) * 4096:
            raise RuntimeError('device reads did not cover the attributed swapped pages')
        captured.extend(f'PAGEOUT_BLOCK_READ_BYTES delta={read_delta}\n'.encode())
        (evidence / 'pageout.log').write_bytes(captured)
        print(captured.decode(), end='', flush=True)
        print('HOST_PAGEOUT_DRAIN_READBACK_PASS bytes=16777216', flush=True)
    finally:
        try:
            (evidence / 'pageout.log').write_bytes(captured)
        except OSError as error:
            print('PAGEOUT_LOG_WRITE_FAILED ' + str(error), flush=True)
        try:
            if worker is not None and worker.poll() is None:
                worker.kill()  # Disposable bounded-memory helper, never the target.
                worker.wait()
        finally:
            try:
                if group is not None and group.exists():
                    group.rmdir()
            finally:
                if active:
                    # The outer owner must keep target/backing alive if evacuation fails.
                    subprocess.run(['/usr/bin/swapoff', DEVICE], check=True)
