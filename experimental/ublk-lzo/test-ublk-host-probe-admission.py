#!/usr/bin/env python3
"""Probe argument/disk budget checks; never creates files or touches a device."""
import contextlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

source = Path(__file__).with_name('test-ublk-host-probe.py')
spec = importlib.util.spec_from_file_location('host_probe', source)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def reject(args, free_bytes, expected):
    with mock.patch('sys.argv', [str(source), '--run', *args]), \
         mock.patch.object(probe.shutil, 'disk_usage', return_value=SimpleNamespace(free=free_bytes)), \
         mock.patch.object(probe.os, 'geteuid', return_value=1000), \
         mock.patch.object(probe.tempfile, 'mkdtemp', side_effect=AssertionError('unexpected file creation')), \
         mock.patch.object(probe.subprocess, 'check_output', side_effect=AssertionError('unexpected system query')), \
         contextlib.redirect_stderr(io.StringIO()):
        try:
            probe.main()
        except SystemExit as error:
            assert error.code == expected, (args, error.code, expected)
        else:
            raise AssertionError('probe admitted an invalid test fixture')


for mib in (2, 64, 256, 1024, 2048, 4096, 8192, 12288, 16384):
    required = 4 * mib * 1024 * 1024 + (1 << 30)
    reject(['--payload-mib', str(mib)], required - 1,
           'insufficient disk headroom for double backing plus 1 GiB')
    reject(['--payload-mib', str(mib)], required, 'root required')
reject(['--payload-mib', '3'], 1 << 40, 2)
reject(['--payload-mib', '-1'], 1 << 40, 2)
reject(['--swap-pageout'], 1 << 40, 2)
reject(['--swap-pageout', '--payload-mib', '64'], 1 << 40, 2)
reject(['--swap-pageout', '--payload-mib', '64', '--vmm', '--during-drain'], 1 << 40, 2)
reject(['--swap-pageout', '--payload-mib', '64', '--vmm'], 1 << 40, 'root required')
reject(['--rearm-cycles', '1'], 1 << 40, 2)
reject(['--rearm-cycles', '4'], 1 << 40, 2)
reject(['--vmm', '--rearm-cycles', '2'], 1 << 40, 'root required')
reject(['--swap-pageout', '--payload-mib', '64', '--vmm', '--rearm-cycles', '1'], 1 << 40, 2)
reject(['--logical-mib', '3'], 1 << 40, 2)
reject(['--logical-mib', '32769'], 1 << 40, 2)
large = ['--vmm', '--payload-mib', '16384', '--logical-mib', '8192']
reject(large, 17 * 1024**3 - 1, 'insufficient disk headroom for double backing plus 1 GiB')
reject(large, 17 * 1024**3, 'root required')
reject(['--swap-pageout', '--payload-mib', '64', '--vmm', '--logical-mib', '4'], 1 << 40, 2)
print('HOST_PROBE_ADMISSION_PASS cases=33 GPU=0 devices=0 swap=0 files_created=0')
