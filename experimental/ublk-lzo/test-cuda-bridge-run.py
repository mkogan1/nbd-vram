#!/usr/bin/env python3
"""Bounded bridge probe; fixture by default, real GPU only with --real-cuda."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
UBLK_SRC = Path(os.environ.get('UBLK_SRC', '.'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--real-cuda', action='store_true')
    parser.add_argument('--vm', action='store_true', help='run ublk and swap only in a disposable VM')
    parser.add_argument('--retire-enospc', action='store_true', help='VM only: exhaust Btrfs, release emergency reserve and retire GPU while swapped pages are held')
    parser.add_argument('--auto-retire', action='store_true', help='VM only: synthetic low-free report triggers retirement, with Btrfs ENOSPC')
    parser.add_argument('--kernel-safe-stop', action='store_true', help='VM only: require kernel opener exclusion for shutdown')
    parser.add_argument('--low-free-after-stop', action='store_true')
    args = parser.parse_args()
    if args.low_free_after_stop and (not args.auto_retire or not args.kernel_safe_stop):
        parser.error('--low-free-after-stop requires --auto-retire and --kernel-safe-stop')
    if args.kernel_safe_stop and not args.vm:
        parser.error('--kernel-safe-stop requires --vm')
    if args.retire_enospc and not args.vm:
        parser.error('--retire-enospc requires --vm')
    if args.auto_retire and (not args.vm or args.retire_enospc):
        parser.error('--auto-retire requires --vm without manual retirement')
    if os.geteuid() == 0:
        parser.error('run as an unprivileged user')
    with tempfile.TemporaryDirectory(prefix='cuda-bridge-check-', dir='/var/tmp') as name:
        work = Path(name)
        def build(output, source, *flags):
            subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(HERE / source), *flags, '-o', str(work / output)], check=True)
        build('server', 'test-cuda-bridge-server.c', '-ldl')
        build('libcuda-bridge.so.1', 'test-libcuda-bridge.c', '-shared', '-fPIC', '-pthread')
        build('probe', 'test-cuda-bridge-roundtrip.c', '-ldl')
        build('libcuda-fixture.so.1', 'test-libcuda-fixture.c', '-shared', '-fPIC')
        driver = Path('/usr/lib/libcuda.so.1') if args.real_cuda else work / 'libcuda-fixture.so.1'
        def start():
            path = work / 'bridge.sock'
            command = [str(work / 'server'), '--socket', str(path), '--driver', str(driver)]
            if args.real_cuda:
                command.append('--real-cuda')
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 5
            while not path.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    process.kill()
                    raise RuntimeError('server startup failed: ' + str(process.communicate()))
                time.sleep(0.01)
            return process, path
        process, path = start()
        try:
            env = dict(os.environ, CUDA_BRIDGE_SOCKET=str(path))
            env.pop('CUDA_BRIDGE_DEVICE', None)
            command = [str(work / 'probe'), str(work / 'libcuda-bridge.so.1')]
            if args.vm:
                command = ['python3', str(HERE / 'ublk-real-integration-vm.py'),
                           '--target', str(HERE / 'ublk-lzo-target'),
                           '--target-arg=--cuda', '--target-arg=--device', '--target-arg=0',
                           '--target-arg=--size', '--target-arg=67108864',
                           '--target-arg=--backing', '--target-arg=/mnt/back',
                           '--include', str(work / 'libcuda-bridge.so.1') + ':/usr/lib/libcuda.so.1',
                           '--include', '/usr/lib/liblzo2.so.2:/usr/lib/liblzo2.so.2',
                           '--library-dir', str(UBLK_SRC / 'lib/.libs'),
                           '--cuda-bridge-socket', str(path), '--mode', 'positive',
                           '--capacity', '67108864', '--backing-fs', 'btrfs',
                           '--swap-pressure-mib', '32', '--require-backing-io',
                           '--kernel-release', '7.3.0-4-cachyos',
                           '--guard-message', 'ublk guest guard rejected environment',
                           '--timeout', '120']
                if args.retire_enospc:
                    command += ['--fill-backing-filesystem', '--expect-emergency-release',
                                '--drain-with-swapped-pages', '--swap-cycles', '2']
                if args.kernel_safe_stop:
                    command += ['--target-arg=--kernel-safe-stop']
                if args.auto_retire:
                    command += ['--fill-backing-filesystem', '--expect-emergency-release',
                                '--cuda-bridge-low-free', '--swap-cycles', '2']
                if args.low_free_after_stop:
                    command += ['--low-free-after-stop']
            result = subprocess.run(command, env=env, capture_output=True, text=True,
                                    timeout=150 if args.vm else 30)
            output, errors = process.communicate(timeout=12)
            print(result.stdout + result.stderr + output + errors, end='')
            if result.returncode or process.returncode:
                raise RuntimeError('bridge roundtrip failed')
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        if not args.real_cuda and not args.vm:
            # Framing must fail closed before any driver operation.
            packets = [struct.pack('=6I5Q', magic, 1, 1, length, 0, 0, 1, 0, 0, 0, 0)
                       for magic, length in ((0, 0), (0x43425231, 65537))]
            packets.append(b'partial')
            for packet in packets:
                process, path = start()
                try:
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.connect(str(path))
                        peer.sendall(packet)
                    process.communicate(timeout=12)
                    if process.returncode == 0:
                        raise RuntimeError('malformed frame was accepted')
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
            print('CUDA_BRIDGE_FRAMING_PASS malformed=3 backend=fixture')
        print('CUDA_BRIDGE_PROBE_VERIFIED backend=' + ('real' if args.real_cuda else 'fixture') +
              ' host_block_devices=0 host_swap=0 payload_cap=2097152')


if __name__ == '__main__':
    main()
