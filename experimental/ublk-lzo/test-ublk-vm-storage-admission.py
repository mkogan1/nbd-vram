#!/usr/bin/env python3
"""Ensure bad capacities/low disk space stop before kernel queries or VM writes."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('vram_vm', HERE / 'ublk-real-integration-vm.py')
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)


class Admitted(Exception):
    pass


def check(capacity, free_bytes, admitted, diagnostic='', extra=()):
    arguments = ['test', '--target', '/nonexistent-test-target', '--mode', 'positive',
                 '--capacity', str(capacity), '--kernel-release', 'unused', '--guard-message', 'guard']
    arguments.extend(extra)
    errors = io.StringIO()
    with patch.object(sys, 'argv', arguments), contextlib.redirect_stderr(errors), \
         patch.object(vm.os, 'statvfs', return_value=SimpleNamespace(f_bavail=free_bytes // 4096, f_frsize=4096)), \
         patch.object(vm.subprocess, 'check_output', side_effect=Admitted) as metadata:
        try:
            vm.main()
        except Admitted:
            if not admitted:
                raise AssertionError('invalid request reached a kernel query')
        except SystemExit as error:
            if admitted or error.code != 2 or diagnostic not in errors.getvalue():
                raise AssertionError('wrong storage admission result') from error
            metadata.assert_not_called()
        else:
            raise AssertionError('test escaped the metadata sentinel')


def main():
    mib = 1024**2
    check(28671, 20 * 1024**3, False, '--capacity must be')
    check(2 * 1024**3 + 4096, 20 * 1024**3, False, '--capacity must be')
    for capacity, disk in ((131072, 512 * mib), (512 * mib, 1536 * mib), (2 * 1024**3, 4608 * mib)):
        threshold = disk + 4 * 1024**3
        check(capacity, threshold - 4096, False, 'free host storage')
        check(capacity, threshold, True)
    check(131072, 20 * 1024**3, False, '--nvidia-ordering requires', ('--nvidia-ordering',))
    check(131072, 20 * 1024**3, False, '--nvidia-ordering requires',
          ('--nvidia-ordering', '--systemd-lifecycle'))
    check(131072, 20 * 1024**3, False, '--sleep-hook-failure requires',
          ('--nvidia-ordering', '--systemd-lifecycle', '--kernel-suspend', '--sleep-hook-failure'))
    check(131072, 20 * 1024**3, True, extra=(
        '--nvidia-ordering', '--systemd-lifecycle', '--sleep-hook-failure',
        '--managed-swap-lifecycle', '--nvkvm-module', '/nonexistent-module',
        '--qemu', '/nonexistent-custom-qemu', '--swap-pressure-mib', '16',
        '--target-arg=--cuda', '--target-arg=--managed-swap', '--target-arg=--kernel-safe-stop'))
    print('UBLK_VM_STORAGE_ADMISSION_PASS cases=12 GPU=0 devices=0 swap=0 files_created=0')


if __name__ == '__main__':
    main()
