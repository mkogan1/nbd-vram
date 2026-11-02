#!/usr/bin/env python3
"""Check safe, non-privileged staging of the ublk service files."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
BIN = HERE / 'ublk-lzo-target'
EXPECTED = {
    'usr/local/libexec/vram-swap/ublk-lzo-target': 0o755,
    'usr/local/libexec/vram-swap/vram-swap-ublk-sleep.sh': 0o755,
    'etc/systemd/system/vram-swap-ublk.service': 0o644,
    'etc/systemd/system/vram-swap-ublk-suspend.service': 0o644,
    'etc/modules-load.d/vram-swap-ublk.conf': 0o644,
    'usr/share/doc/vram-swap-ublk/vram-swap-ublk.conf.example': 0o644,
}


def run_make(destdir, env=None):
    args = ['make', '-o', 'ublk-lzo-target', 'stage']
    if destdir is not None:
        args.append('DESTDIR=' + str(destdir))
    return subprocess.run(args, cwd=HERE, env=env, text=True, capture_output=True)


def expect_rejected_before_install(destdir, install_log):
    with tempfile.TemporaryDirectory(prefix='vram-stage-bin-', dir='/var/tmp') as temp:
        bindir = Path(temp)
        shim = bindir / 'install'
        shim.write_text('#!/bin/sh\nprintf called >> "$INSTALL_LOG"\nexec /usr/bin/install "$@"\n')
        shim.chmod(0o755)
        env = os.environ.copy()
        env['PATH'] = str(bindir) + os.pathsep + env['PATH']
        env['INSTALL_LOG'] = str(install_log)
        result = run_make(destdir, env)
    assert result.returncode != 0, f'make stage unexpectedly accepted DESTDIR={destdir!r}'
    assert 'DESTDIR must name a non-root staging directory' in result.stderr, result.stderr
    assert not install_log.exists(), f'install ran before rejecting DESTDIR={destdir!r}'


def main():
    assert BIN.is_file(), f'prebuilt binary missing: {BIN}'
    with tempfile.TemporaryDirectory(prefix='vram-ublk-stage-', dir='/var/tmp') as temp:
        root = Path(temp) / 'stage'
        result = run_make(root)
        assert result.returncode == 0, result.stdout + result.stderr

        found = {str(path.relative_to(root)) for path in root.rglob('*') if path.is_file() or path.is_symlink()}
        assert found == set(EXPECTED), f'staged files differ: missing={set(EXPECTED)-found}, extra={found-set(EXPECTED)}'
        for rel, mode in EXPECTED.items():
            path = root / rel
            assert not path.is_symlink(), f'unexpected symlink: {rel}'
            assert path.stat().st_mode & 0o777 == mode, f'wrong mode for {rel}: {path.stat().st_mode & 0o777:o}'
        assert not list(root.rglob('*.wants')), 'staging must not create systemd .wants links'
        assert not (root / 'etc/vram-swap-ublk.conf').exists(), 'staging must not install active /etc config'

        service = (root / 'etc/systemd/system/vram-swap-ublk.service').read_text()
        suspend = (root / 'etc/systemd/system/vram-swap-ublk-suspend.service').read_text()
        libexec = '/usr/local/libexec/vram-swap/'
        assert re.search(r'^ExecStart=' + re.escape(libexec + 'ublk-lzo-target') + r'(?=\s|$)', service, re.M)
        assert re.search(r'^Exec(?:Start|Stop)=' + re.escape(libexec + 'vram-swap-ublk-sleep.sh') + r'(?=\s|$)', suspend, re.M)
        assert '--cuda-vmm' in service
        assert 'nvidia-resume.service' in re.search(r'^After=(.*)$', service, re.M)[1].split()
        for dependency in ('sleep.target', 'nvidia-suspend.service', 'nvidia-hibernate.service'):
            assert dependency in re.search(r'^Before=(.*)$', suspend, re.M)[1].split()
            assert dependency in re.search(r'^RequiredBy=(.*)$', suspend, re.M)[1].split()

        dynamic = subprocess.check_output(['readelf', '-d', str(root / next(p for p in EXPECTED if p.endswith('/ublk-lzo-target')))], text=True)
        assert not re.search(r'\((?:RPATH|RUNPATH)\)', dynamic), 'staged binary has RPATH/RUNPATH'
        assert 'Shared library: [libublksrv.so' not in dynamic, 'staged binary depends on libublksrv.so'

        noarg_log = Path(temp) / 'noarg-install.log'
        expect_rejected_before_install(None, noarg_log)
        for value, label in (('', 'empty'), ('/', 'root')):
            log = Path(temp) / f'{label}-install.log'
            expect_rejected_before_install(value, log)
        link = Path(temp) / 'root-link'
        link.symlink_to('/')
        link_log = Path(temp) / 'symlink-install.log'
        expect_rejected_before_install(link, link_log)

    print('UBLK_STAGE_PASS files=6 modes=ok paths=ok ELF=ok unsafe_DESTDIR=4 no_root=1 no_GPU=1')


if __name__ == '__main__':
    main()
