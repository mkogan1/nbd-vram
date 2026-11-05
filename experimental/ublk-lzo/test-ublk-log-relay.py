#!/usr/bin/env python3
"""Check the exact guest log relay without starting a VM or touching devices."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    init = (HERE / 'ublk-real-integration-init').read_text()
    relay = next(line for line in init.splitlines() if line.startswith('awk -v logfile='))
    program = relay.split("'", 2)[1]
    data = ''.join(f'UBLK_BACKING_IO op=read page={i}\n' for i in range(5000)).encode()
    with tempfile.TemporaryDirectory(prefix='ublk-relay-', dir='/var/tmp') as name:
        logfile = Path(name) / 'captured'
        busybox = os.environ.get('BUSYBOX') or shutil.which('busybox')
        if not busybox:
            raise SystemExit('BusyBox required: set BUSYBOX or install it on PATH')
        result = subprocess.run([busybox, 'awk',
                                 '-v', f'logfile={logfile}', program], input=data,
                                capture_output=True, timeout=10, check=True)
        if result.stdout != data or logfile.read_bytes() != data:
            raise AssertionError('guest relay lost or changed diagnostic lines')
    print('UBLK_LOG_RELAY_PASS lines=5000 GPU=0 devices=0 swap=0')


if __name__ == '__main__':
    main()
