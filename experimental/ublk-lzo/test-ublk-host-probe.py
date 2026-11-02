#!/usr/bin/env python3
"""Explicit disposable ublk probe; swap requires the separate --swap-pageout flag."""
import argparse
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import tempfile
import time
import shutil
import re

HERE = Path(__file__).resolve().parent
DEVICE = 31


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def swaps():
    return sorted(tuple(row.split()[:3] + row.split()[4:])
                  for row in Path('/proc/swaps').read_text().splitlines()[1:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true', required=True)
    parser.add_argument('--payload-mib', type=int, choices=(2, 64, 256, 1024, 2048, 4096, 8192, 12288, 16384), default=2)
    parser.add_argument('--logical-mib', type=int, help='bounded logical dataset; default twice GPU capacity')
    parser.add_argument('--during-drain', action='store_true', help='require a verified read before retirement finishes')
    parser.add_argument('--vmm', action='store_true', help='use independently releasable CUDA physical blocks')
    parser.add_argument('--swap-pageout', action='store_true', help='bounded 16 MiB pageout in a private cgroup; temporary swap')
    parser.add_argument('--rearm-cycles', type=int, choices=(0, 1, 2, 3), default=0, help='verify demand growth after repeated retirement')
    args = parser.parse_args()
    if args.logical_mib is not None and not 4 <= args.logical_mib <= 32768:
        parser.error('--logical-mib must be 4..32768')
    if args.rearm_cycles and (not args.vmm or args.swap_pageout):
        parser.error('--rearm-cycles requires --vmm without --swap-pageout')
    if args.swap_pageout and (args.payload_mib != 64 or args.during_drain or not args.vmm):
        parser.error('--swap-pageout requires --payload-mib 64 --vmm without --during-drain')
    if args.during_drain and args.payload_mib < 64:
        parser.error('--during-drain requires at least 64 MiB payload')
    payload = args.payload_mib * 1024 * 1024
    capacity = args.logical_mib * 1024 * 1024 if args.logical_mib is not None else 2 * payload
    if args.swap_pageout and capacity != 128 * 1024 * 1024:
        parser.error('--swap-pageout requires 128 MiB logical capacity')
    if shutil.disk_usage('/var/tmp').free < 2 * capacity + (1 << 30):
        raise SystemExit('insufficient disk headroom for double backing plus 1 GiB')
    if os.geteuid() != 0:
        raise SystemExit('root required')
    if list(Path('/sys/class/block').glob('ublkb*')) or list(Path('/dev').glob('ublkc*')):
        raise SystemExit('existing ublk devices: refusing shared namespace')
    state = subprocess.check_output(['/usr/bin/systemctl', 'show', 'vram-swap-nbd.service',
                                     '--property=ActiveState', '--value'], text=True).strip()
    if state != 'inactive':
        raise SystemExit('installed NBD service must already be inactive')
    before = swaps()
    loaded = Path('/sys/module/ublk_drv').exists()
    evidence = Path(tempfile.mkdtemp(prefix='vram-native-probe-', dir='/var/tmp'))
    print('HOST_PROBE_EVIDENCE ' + str(evidence), flush=True)
    process = None
    runtime = Path('/run/ublksrvd')
    made_runtime = False
    stopped = True
    try:
        if not loaded:
            run(['/usr/bin/modprobe', 'ublk_drv'])
        if not runtime.exists():
            runtime.mkdir(mode=0o755)
            made_runtime = True
        info = runtime.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError('unsafe ublk runtime directory')
        resource.setrlimit(resource.RLIMIT_MEMLOCK, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
        log = evidence / 'target.log'
        with log.open('wb') as output:
            process = subprocess.Popen([str(HERE / 'ublk-lzo-target'), '--host', '--cuda',
                '--kernel-safe-stop', '--device', str(DEVICE), '--size', str(capacity),
                '--gpu-payload-bytes', str(payload),
                '--backing', str(evidence)] + (['--cuda-vmm'] if args.vmm else []), stdout=output, stderr=subprocess.STDOUT,
                env={'PATH': '/usr/bin:/bin', 'LANG': 'C'})
            stopped = False

            def wait_log(marker, since=0):
                deadline = time.monotonic() + 120 + args.payload_mib / 4
                while True:
                    text = log.read_text(errors='replace')
                    if 'UBLK_DRAIN_FAILED ' in text or 'UBLK_STORE_ERROR ' in text:
                        raise RuntimeError('target reported a storage/drain failure')
                    if marker in text[since:]:
                        break
                    if process.poll() is not None:
                        raise RuntimeError('target exited before ' + marker)
                    if time.monotonic() >= deadline:
                        raise TimeoutError('waiting for ' + marker)
                    time.sleep(0.1)

            wait_log('UBLK_LZO_READY /dev/ublkb31')
            print(f'HOST_PROBE_READY payload={payload} logical={capacity}', flush=True)
            if args.swap_pageout:
                from host_pageout_probe import run_pageout
                run_pageout(process, evidence, wait_log)
            else:
                with (evidence / 'io.log').open('wb') as io_log:
                    started = time.monotonic()
                    run([str(HERE / 'test-ublk-host-io'), str(DEVICE), str(process.pid), str(capacity)],
                        stdout=io_log, stderr=subprocess.STDOUT, timeout=120 + capacity // (4 * 1024 * 1024))
                    print(f'HOST_PROBE_WRITE_READ_PASS elapsed_s={time.monotonic() - started:.3f}', flush=True)
                    started = time.monotonic()
                    drain_elapsed = None
                    last_drain_start = len(log.read_text(errors='replace'))
                    process.send_signal(signal.SIGUSR2)
                    if args.during_drain:
                        wait_log('UBLK_DRAIN_STARTED ')
                        concurrent_log = evidence / 'concurrent.log'
                        with concurrent_log.open('wb') as concurrent_output:
                            reader = subprocess.Popen([str(HERE / 'test-ublk-host-io'), str(DEVICE),
                                str(process.pid), str(capacity), '--verify-only'],
                                stdout=concurrent_output, stderr=subprocess.STDOUT)
                            try:
                                deadline = time.monotonic() + 30
                                while 'HOST_UBLK_FIRST_PAGE_VERIFIED' not in concurrent_log.read_text(errors='replace'):
                                    if reader.poll() is not None or time.monotonic() >= deadline:
                                        raise RuntimeError('concurrent reader did not verify its first page')
                                    time.sleep(0.01)
                                if 'UBLK_DRAIN_OK ' in log.read_text(errors='replace'):
                                    raise RuntimeError('read progress was not observed before drain completed')
                                print('HOST_PROBE_READ_DURING_DRAIN_PASS', flush=True)
                                wait_log('UBLK_DRAIN_OK ')
                                drain_elapsed = time.monotonic() - started
                                if reader.wait(timeout=120 + capacity // (4 * 1024 * 1024)) != 0:
                                    raise RuntimeError('concurrent full-device verification failed')
                            finally:
                                if reader.poll() is None:
                                    reader.kill()  # Only the disposable I/O fixture, never the target.
                                    reader.wait()
                    else:
                        wait_log('UBLK_DRAIN_OK ')
                    if drain_elapsed is None:
                        drain_elapsed = time.monotonic() - started
                    print(f'HOST_PROBE_DRAIN_PASS elapsed_s={drain_elapsed:.3f}', flush=True)
                    if args.vmm and args.payload_mib >= 64:
                        early = re.search(r'^UBLK_VMM_EARLY_RELEASE bytes=(\d+) cursor=(\d+) pages=(\d+)$',
                                          log.read_text(errors='replace'), re.MULTILINE)
                        if not early or not (0 < int(early[1]) < payload and int(early[2]) < int(early[3])):
                            raise RuntimeError('partial VRAM release before migration completion was not proven')
                        print('HOST_PROBE_EARLY_VRAM_RELEASE_PASS bytes=' + early[1], flush=True)
                    started = time.monotonic()
                    run([str(HERE / 'test-ublk-host-io'), str(DEVICE), str(process.pid), str(capacity), '--verify-only'],
                        stdout=io_log, stderr=subprocess.STDOUT, timeout=120 + capacity // (4 * 1024 * 1024))
                    print(f'HOST_PROBE_RETIRED_READ_PASS elapsed_s={time.monotonic() - started:.3f}', flush=True)
                    for cycle in range(1, args.rearm_cycles + 1):
                        wait_log('UBLK_VMM_REARMED ', since=last_drain_start)
                        cycle_start = len(log.read_text(errors='replace'))
                        run([str(HERE / 'test-ublk-host-io'), str(DEVICE), str(process.pid), str(capacity)],
                            stdout=io_log, stderr=subprocess.STDOUT, timeout=120 + capacity // (4 * 1024 * 1024))
                        last_drain_start = len(log.read_text(errors='replace'))
                        process.send_signal(signal.SIGUSR2)
                        wait_log('UBLK_DRAIN_OK ', since=last_drain_start)
                        references = re.search(r'^UBLK_VMM_REFERENCES compressed=(\d+) raw=(\d+)$',
                                               log.read_text(errors='replace')[cycle_start:], re.MULTILINE)
                        if not references or int(references[1]) + int(references[2]) == 0:
                            raise RuntimeError('rearmed writes did not populate GPU memory')
                        run([str(HERE / 'test-ublk-host-io'), str(DEVICE), str(process.pid), str(capacity), '--verify-only'],
                            stdout=io_log, stderr=subprocess.STDOUT, timeout=120 + capacity // (4 * 1024 * 1024))
                        print(f'HOST_PROBE_REARM_CYCLE_PASS cycle={cycle}', flush=True)
    finally:
        if process is not None:
            for _ in range(30):
                if process.poll() is not None:
                    break
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            stopped = process.poll() is not None
            if not stopped:
                print(f'HOST_PROBE_RETAINED pid={process.pid} evidence={evidence} no_sigkill=1', flush=True)
        if stopped:
            if made_runtime:
                try:
                    runtime.rmdir()
                except OSError:
                    pass  # Never remove metadata left by another process.
            if not loaded and Path('/sys/module/ublk_drv').exists():
                run(['/usr/bin/modprobe', '-r', 'ublk_drv'])
    if not stopped:
        raise RuntimeError('target retained; retry TERM after its kernel users detach')
    if process.returncode != 0:
        raise RuntimeError(f'target exit status {process.returncode}; evidence={evidence}')
    if Path('/sys/class/block/ublkb31').exists() or Path('/dev/ublkc31').exists():
        raise RuntimeError('probe device remains')
    if before != swaps():
        raise RuntimeError('host swap identities changed during probe')
    if 'UBLK_STOP_DONE' not in log.read_text(errors='replace'):
        raise RuntimeError('kernel stop was not confirmed')
    print(f'HOST_NATIVE_PROBE_PASS payload={payload} logical={capacity} swap_changes=0 evidence={evidence}')


if __name__ == '__main__':
    main()
