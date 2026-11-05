#!/usr/bin/env python3
"""CPU-only checks of the direct-I/O device digest using a private regular file."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def expected(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'UBLK_DEVICE_DIGEST bytes={len(data)} fnv1a64={value:016x}\n'


def main():
    with tempfile.TemporaryDirectory(prefix='vram-digest-test-', dir='/var/tmp') as directory:
        work = Path(directory)
        binary, datafile = work / 'probe', work / 'data'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                        str(HERE / 'test-ublk-real-io.c'), '-o', str(binary)], check=True)
        data = bytearray((i * 37 + i // 4096) % 256 for i in range(7 * 4096))
        for change in (False, True):
            if change:
                data[-1] ^= 1
            datafile.write_bytes(data)
            actual = subprocess.check_output([str(binary), str(datafile), str(len(data)), '--digest-only'], text=True)
            if actual != expected(data):
                raise AssertionError('direct-I/O digest did not match independent byte calculation')
        invalid = subprocess.run([str(binary), str(datafile), str(len(data) + 4096), '--digest-only'], capture_output=True)
        if invalid.returncode == 0:
            raise AssertionError('wrong capacity was accepted')
    print('UBLK_DEVICE_DIGEST_PASS cases=3 GPU=0 devices=0 swap=0')


if __name__ == '__main__':
    main()
