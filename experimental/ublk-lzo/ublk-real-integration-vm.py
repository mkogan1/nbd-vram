#!/usr/bin/env python3
"""Run an explicitly supplied guest target, never on the host."""
import argparse
import gzip
import os
import re
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tempfile
import threading
import importlib.util

HERE = Path(__file__).resolve().parent
UBLK_SRC = Path(os.environ.get('UBLK_SRC', '.'))


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', required=True, type=Path, help='host target executable, copied to /usr/bin/target')
    parser.add_argument('--target-arg', action='append', default=[], help='guest target argument; repeat in order')
    parser.add_argument('--include', action='append', default=[], metavar='HOST:GUEST', help='extra executable or data file')
    parser.add_argument('--library-dir', action='append', default=[], help='host library search directory, for ELF dependencies')
    parser.add_argument('--kernel', type=Path, default=Path('/boot/vmlinuz'))
    parser.add_argument('--qemu', type=Path, default=Path('/usr/bin/qemu-system-x86_64'))
    parser.add_argument('--nvkvm-module', type=Path, help='guest-only compute module; requires explicit custom QEMU and target --cuda')
    parser.add_argument('--busybox', type=Path, default=Path(os.environ.get('BUSYBOX') or shutil.which('busybox') or '/usr/bin/busybox'))
    parser.add_argument('--mode', required=True, choices=['positive', 'no-marker', 'no-hypervisor', 'other-dmi'])
    parser.add_argument('--capacity', required=True, type=int, help='exact target capacity in bytes')
    parser.add_argument('--backing-fs', choices=['ext4', 'btrfs'], default='ext4')
    parser.add_argument('--btrfs-sectorsize', type=int, choices=[4096, 16384], default=4096)
    parser.add_argument('--require-backing-io', action='store_true', help='require target evidence of disk writes and reads')
    parser.add_argument('--require-ready-notification', action='store_true', help='verify target READY datagram after block device startup')
    parser.add_argument('--ready-notification-failure', action='store_true', help='verify safe cleanup when READY cannot be delivered')
    parser.add_argument('--managed-swap-lifecycle', action='store_true', help='target activates swap and evacuates live pages on TERM')
    parser.add_argument('--managed-swapoff-failure-once', action='store_true', help='guest utility refuses first managed swapoff, then forwards retry')
    parser.add_argument('--systemd-lifecycle', action='store_true', help='boot actual systemd and test the staged unit and sleep hook')
    parser.add_argument('--kernel-suspend', action='store_true', help='suspend only the guest kernel and wake it through its private QMP socket')
    parser.add_argument('--nvidia-ordering', action='store_true', help='exercise NVIDIA suspend/resume ordering in the isolated guest fixture')
    parser.add_argument('--sleep-hook-failure', action='store_true', help='verify failed pre-sleep hook blocks guest suspension')
    parser.add_argument('--swap-pressure-mib', type=int, default=0)
    parser.add_argument('--swap-cycles', type=int, choices=range(1, 11), default=1)
    parser.add_argument('--drain-before-pageout', action='store_true')
    parser.add_argument('--drain-with-swapped-pages', action='store_true')
    parser.add_argument('--fill-backing-filesystem', action='store_true')
    parser.add_argument('--reclaim-filler-mib', type=int, choices=[0, 64], default=0)
    parser.add_argument('--backing-probe', action='store_true', help='run guest target as CPU-only backing probe before loading ublk')
    parser.add_argument('--expect-cuda-fixture', action='store_true', help='explicitly label fake CUDA driver integration')
    parser.add_argument('--cuda-bridge-socket', type=Path, help='existing private host CUDA bridge socket; never starts a GPU server')
    parser.add_argument('--cuda-bridge-low-free', action='store_true', help='synthetic pressure report, real bridge backend unchanged')
    parser.add_argument('--cuda-native-low-free', action='store_true', help='VM-only synthetic pressure report with unmodified NVIDIA userspace')
    parser.add_argument('--low-free-after-stop', action='store_true', help='trigger synthetic pressure only after safe STOP was rejected busy')
    parser.add_argument('--cuda-fixture-init-failure', action='store_true')
    parser.add_argument('--cuda-fixture-low-free', action='store_true')
    parser.add_argument('--stop-with-held-io', action='store_true')
    parser.add_argument('--stop-failure-once', action='store_true', help='VM-only preload rejects first STOP without calling the kernel')
    parser.add_argument('--stop-lost-reply', action='store_true', help='VM-only real STOP followed by synthetic error')
    parser.add_argument('--start-params-failure', action='store_true', help='VM-only reject parameters after queue creation')
    parser.add_argument('--readonly-probe', action='store_true', help='VM-only investigate kernel read-only gate during swap recovery')
    parser.add_argument('--safe-stop-probe', action='store_true', help='VM-only kernel TRY_STOP test with direct, loop and unrelated swap')
    parser.add_argument('--safe-stop-target', action='store_true', help='VM-only full target safe-stop integration')
    parser.add_argument('--expect-emergency-release', action='store_true')
    parser.add_argument('--reclaim-pressure-mib', type=int, default=0)
    parser.add_argument('--kernel-release', required=True, help='exact release of guest kernel and module')
    parser.add_argument('--guard-message', required=True, help='target diagnostic proving guard rejection')
    parser.add_argument('--timeout', type=int, default=75)
    args = parser.parse_args()
    if args.sleep_hook_failure and (not args.systemd_lifecycle or args.kernel_suspend):
        parser.error('--sleep-hook-failure requires --systemd-lifecycle without actual kernel suspend')
    if args.kernel_suspend and not args.systemd_lifecycle:
        parser.error('--kernel-suspend requires --systemd-lifecycle')
    if args.nvidia_ordering and (not args.systemd_lifecycle or not (args.kernel_suspend or args.sleep_hook_failure)):
        parser.error('--nvidia-ordering requires --systemd-lifecycle and either --kernel-suspend or --sleep-hook-failure')
    if args.systemd_lifecycle and (not args.managed_swap_lifecycle or not args.nvkvm_module or args.require_ready_notification or args.managed_swapoff_failure_once or args.fill_backing_filesystem):
        parser.error('--systemd-lifecycle requires managed native CUDA without external notification listener, fault or filesystem filler')
    if args.managed_swapoff_failure_once and not args.managed_swap_lifecycle:
        parser.error('--managed-swapoff-failure-once requires --managed-swap-lifecycle')
    if ('--managed-swap' in args.target_arg) != args.managed_swap_lifecycle:
        parser.error('target --managed-swap requires --managed-swap-lifecycle and vice versa')
    if args.managed_swap_lifecycle and (args.mode != 'positive' or '--kernel-safe-stop' not in args.target_arg or not (args.require_ready_notification or args.systemd_lifecycle) or not args.swap_pressure_mib or args.swap_cycles != 1 or args.ready_notification_failure or args.start_params_failure or args.backing_probe or args.safe_stop_probe or args.safe_stop_target or args.stop_with_held_io or args.stop_failure_once or args.stop_lost_reply or args.drain_before_pageout or args.drain_with_swapped_pages or args.readonly_probe or args.cuda_fixture_init_failure or args.cuda_fixture_low_free or args.cuda_bridge_low_free or args.cuda_native_low_free):
        parser.error('--managed-swap-lifecycle requires positive safe-stop, readiness and one pressure cycle without other lifecycle modes')
    if args.ready_notification_failure and (args.mode != 'positive' or args.require_ready_notification or args.backing_probe or args.start_params_failure or args.cuda_fixture_init_failure or args.swap_pressure_mib or args.fill_backing_filesystem or args.require_backing_io or args.safe_stop_probe or args.safe_stop_target or args.stop_with_held_io or args.stop_failure_once or args.stop_lost_reply):
        parser.error('--ready-notification-failure requires positive startup without other workloads')
    if args.require_ready_notification and (args.mode != 'positive' or args.backing_probe or args.start_params_failure or args.cuda_fixture_init_failure):
        parser.error('--require-ready-notification requires successful positive target startup')
    auto_low_free = args.cuda_fixture_low_free or args.cuda_bridge_low_free or args.cuda_native_low_free
    if args.low_free_after_stop and (not auto_low_free or '--kernel-safe-stop' not in args.target_arg):
        parser.error('--low-free-after-stop requires automatic pressure injection and target --kernel-safe-stop')
    if args.safe_stop_target and (args.safe_stop_probe or '--kernel-safe-stop' not in args.target_arg):
        parser.error('--safe-stop-target requires target --kernel-safe-stop without external probe')
    if (args.safe_stop_probe or args.safe_stop_target) and (args.mode != 'positive' or args.backing_fs != 'ext4' or args.swap_pressure_mib or args.stop_failure_once or args.stop_lost_reply or args.start_params_failure or args.stop_with_held_io or args.readonly_probe or args.backing_probe or auto_low_free):
        parser.error('--safe-stop-probe requires positive ext4 without other lifecycle experiments')
    if args.readonly_probe and (args.mode != 'positive' or not args.swap_pressure_mib or args.swap_cycles != 1 or args.stop_failure_once or args.stop_lost_reply or args.start_params_failure or args.stop_with_held_io or args.backing_probe or auto_low_free or args.drain_before_pageout or args.drain_with_swapped_pages):
        parser.error('--readonly-probe requires one positive swap cycle without lifecycle faults')
    if args.stop_lost_reply and (args.mode != 'positive' or args.stop_failure_once or args.start_params_failure or args.stop_with_held_io or args.backing_probe or args.cuda_fixture_init_failure):
        parser.error('--stop-lost-reply requires positive integration without other lifecycle faults')
    if args.start_params_failure and (args.mode != 'positive' or args.stop_failure_once or args.stop_with_held_io or args.backing_probe or args.swap_pressure_mib or args.fill_backing_filesystem or args.require_backing_io or args.cuda_fixture_init_failure or auto_low_free):
        parser.error('--start-params-failure requires positive startup-only test')
    if args.stop_failure_once and (args.mode != 'positive' or args.stop_with_held_io or args.backing_probe or args.cuda_fixture_init_failure):
        parser.error('--stop-failure-once requires positive full-target integration')
    if args.cuda_native_low_free and (not args.nvkvm_module or not args.swap_pressure_mib or args.drain_before_pageout or args.drain_with_swapped_pages or args.cuda_fixture_low_free or args.cuda_bridge_low_free):
        parser.error('--cuda-native-low-free requires native CUDA swap workload without another drain mode')
    if args.nvkvm_module:
        if args.mode != 'positive' or '--cuda' not in args.target_arg or args.qemu == Path('/usr/bin/qemu-system-x86_64') or args.cuda_bridge_socket or args.expect_cuda_fixture or args.backing_probe or args.cuda_fixture_low_free or args.cuda_bridge_low_free or args.stop_with_held_io:
            parser.error('native guest CUDA requires positive --cuda, custom --qemu and no fixture/bridge')
        native_vermagic = subprocess.check_output(['/usr/bin/modinfo', '-F', 'vermagic', str(args.nvkvm_module)], text=True).split()
        if not native_vermagic or native_vermagic[0] != args.kernel_release:
            parser.error('nvkvm module must match guest kernel release')
        available = re.search(r'^MemAvailable:\s+(\d+) kB$', Path('/proc/meminfo').read_text(), re.M)
        if not available or int(available.group(1)) < 4 * 1024 * 1024:
            parser.error('native CUDA guest requires at least 4 GiB host MemAvailable')
    if args.cuda_bridge_low_free and (not args.cuda_bridge_socket or not args.swap_pressure_mib or args.drain_before_pageout or args.drain_with_swapped_pages):
        parser.error('--cuda-bridge-low-free requires bridge swap workload without manual drain')
    if args.drain_with_swapped_pages and (args.mode != 'positive' or not args.swap_pressure_mib or args.drain_before_pageout or auto_low_free):
        parser.error('--drain-with-swapped-pages requires positive swap workload without another drain mode')
    if args.cuda_bridge_socket:
        if args.mode != 'positive' or '--cuda' not in args.target_arg or args.backing_probe or args.expect_cuda_fixture or args.stop_with_held_io or args.cuda_fixture_low_free or args.cuda_fixture_init_failure:
            parser.error('CUDA bridge requires positive --cuda without fixture modes')
        args.cuda_bridge_socket = args.cuda_bridge_socket.resolve(strict=True)
        bridge_stat = args.cuda_bridge_socket.stat()
        parent_stat = args.cuda_bridge_socket.parent.stat()
        if not stat.S_ISSOCK(bridge_stat.st_mode) or bridge_stat.st_uid != os.getuid() or parent_stat.st_uid != os.getuid() or parent_stat.st_mode & 0o077:
            parser.error('CUDA bridge socket must belong to this user in a private directory')
        if ',' in str(args.cuda_bridge_socket):
            parser.error('CUDA bridge socket path cannot contain commas')
    if args.expect_emergency_release and (args.mode != 'positive' or not args.fill_backing_filesystem or args.reclaim_filler_mib or args.backing_probe):
        parser.error('--expect-emergency-release requires filesystem fill without external filler reclaim')
    if args.stop_with_held_io and (not args.expect_cuda_fixture or args.swap_pressure_mib or args.drain_before_pageout or args.cuda_fixture_low_free or args.cuda_fixture_init_failure or args.backing_probe):
        parser.error('--stop-with-held-io requires fixture integration without other workloads')
    if args.cuda_fixture_low_free and (not args.expect_cuda_fixture or not args.swap_pressure_mib or args.drain_before_pageout or args.cuda_fixture_init_failure):
        parser.error('--cuda-fixture-low-free requires fixture swap workload without manual drain or init failure')
    if args.cuda_fixture_init_failure and (not args.expect_cuda_fixture or args.swap_pressure_mib or args.drain_before_pageout or args.fill_backing_filesystem):
        parser.error('--cuda-fixture-init-failure requires fixture expectation without workloads')
    if args.expect_cuda_fixture and (args.mode != 'positive' or '--cuda' not in args.target_arg or args.backing_probe):
        parser.error('--expect-cuda-fixture requires positive target --cuda integration')
    if args.backing_probe and (args.mode != 'positive' or args.swap_pressure_mib or args.require_backing_io or args.fill_backing_filesystem or args.drain_before_pageout):
        parser.error('--backing-probe requires positive mode without ublk workload flags')
    if args.reclaim_filler_mib and not args.fill_backing_filesystem:
        parser.error('--reclaim-filler-mib requires --fill-backing-filesystem')
    if args.drain_before_pageout and (not args.swap_pressure_mib or args.mode != 'positive'):
        parser.error('--drain-before-pageout requires positive mode and swap pressure')
    if not 0 <= args.swap_pressure_mib <= 512:
        parser.error('--swap-pressure-mib must be between 0 and 512')
    if not 0 <= args.reclaim_pressure_mib <= 384 or args.reclaim_pressure_mib and not args.swap_pressure_mib:
        parser.error('--reclaim-pressure-mib requires swap pressure and must be between 0 and 384')
    if not 25 <= args.timeout <= 300:
        parser.error('--timeout must be between 25 and 300 seconds')
    if not args.guard_message.strip() or '\n' in args.guard_message:
        parser.error('--guard-message must be a nonempty single line')
    if args.capacity < 28672 or args.capacity > 2 * 1024**3 or args.capacity % 4096:
        parser.error('--capacity must be 28672..2147483648 and 4096-aligned (isolated harness limit)')
    disk_bytes = 512 * 1024**2 if args.capacity <= 128 * 1024**2 else 2 * args.capacity + 512 * 1024**2
    space = os.statvfs('/var/tmp')
    if space.f_bavail * space.f_frsize < disk_bytes + 4 * 1024**3:
        parser.error('disposable guest disk requires its full size plus 4 GiB free host storage')
    kernel_description = subprocess.check_output(['file', '-b', str(args.kernel)], text=True)
    if not re.search(r'\bversion ' + re.escape(args.kernel_release) + r'\s', kernel_description):
        raise SystemExit('guest kernel release cannot be confirmed')
    module = subprocess.check_output(['/usr/bin/modinfo', '-k', args.kernel_release, '-F', 'filename', 'ublk_drv'], text=True).strip()
    vermagic = subprocess.check_output(['/usr/bin/modinfo', '-F', 'vermagic', module], text=True).split()
    if not vermagic or vermagic[0] != args.kernel_release:
        raise SystemExit('module release does not match guest kernel')
    with tempfile.TemporaryDirectory(prefix='ublk-real-vm-', dir='/var/tmp') as temp:
        temp = Path(temp)
        root = temp / 'root'
        for name in ('bin', 'usr/bin', 'usr/lib', 'lib64', 'dev', 'proc', 'sys', 'modules', 'tmp', 'run'):
            (root / name).mkdir(parents=True, exist_ok=True)

        def copy(source, destination, deps=False):
            source = source.resolve(strict=True)
            dest = root / destination.lstrip('/')
            if not dest.resolve().is_relative_to(root):
                raise ValueError('destination escapes initramfs')
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
            dest.chmod(source.stat().st_mode & 0o777)
            if deps:
                # readelf inspects ELF metadata without running the supplied executable.
                header = subprocess.check_output(['readelf', '-l', '-d', str(source)], text=True, stderr=subprocess.STDOUT)
                interpreter = re.search(r'Requesting program interpreter: ([^]]+)', header)
                if interpreter:
                    copy(Path(interpreter.group(1)), interpreter.group(1))
                for needed in re.findall(r'Shared library: \[(.*?)\]', header):
                    candidates = [Path(p) / needed for p in args.library_dir + ['/lib64', '/usr/lib64', '/lib', '/usr/lib', '/lib/x86_64-linux-gnu', '/usr/lib/x86_64-linux-gnu']]
                    lib = next((p for p in candidates if p.is_file()), None)
                    if lib is None:
                        raise RuntimeError('missing ELF dependency: ' + needed)
                    destination = root / 'usr/lib' / needed
                    if not destination.exists():
                        copy(lib, '/usr/lib/' + needed, True)

        copy(args.busybox, '/bin/busybox')
        copy(HERE / 'ublk-real-integration-init', '/init')
        copy(args.target, '/usr/bin/target', True)
        if args.systemd_lifecycle:
            copy(Path('/usr/lib/systemd/systemd'), '/usr/lib/systemd/systemd', True)
            copy(Path('/usr/lib/systemd/systemd-executor'), '/usr/lib/systemd/systemd-executor', True)
            copy(Path('/usr/bin/systemctl'), '/usr/bin/systemctl', True)
            copy(Path('/usr/bin/test'), '/usr/bin/test', True)
            copy(Path('/usr/bin/flock'), '/usr/bin/flock', True)
            if args.kernel_suspend or args.sleep_hook_failure:
                copy(Path('/usr/lib/systemd/systemd-sleep'), '/usr/lib/systemd/systemd-sleep', True)
                for name in ('systemd-suspend.service', 'suspend.target', 'sleep.target'):
                    copy(Path('/usr/lib/systemd/system') / name, '/etc/systemd/system/' + name)
                copy(HERE / 'vram-swap-ublk-suspend.service', '/etc/systemd/system/vram-swap-ublk-suspend.service')
                # The experimental virtio GPU bridge has no freeze/restore callbacks.
                # Rebind only the guest bridge, after evacuation and before restoration.
                (root / 'etc/systemd/system/nvkvm-sleep.service').write_text('[Unit]\nDescription=Isolated guest GPU bridge lifecycle\nDefaultDependencies=no\nAfter=vram-swap-ublk-suspend.service\nBefore=sleep.target\nStopWhenUnneeded=yes\n[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/usr/bin/nvkvm-sleep pre\nExecStop=/usr/bin/nvkvm-sleep post\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\n')
                wants = root / 'etc/systemd/system/sleep.target.requires'
                wants.mkdir()
                (wants / 'vram-swap-ublk-suspend.service').symlink_to('../vram-swap-ublk-suspend.service')
                if args.kernel_suspend:
                    copy(HERE / 'test-nvkvm-sleep.sh', '/usr/bin/nvkvm-sleep')
                    (root / 'usr/bin/nvkvm-sleep').chmod(0o755)
                    (wants / 'nvkvm-sleep.service').symlink_to('../nvkvm-sleep.service')
                else:
                    copy(Path('/usr/bin/false'), '/usr/bin/false', True)
                    failed_hook = root / 'etc/systemd/system/vram-swap-ublk-suspend.service.d'
                    failed_hook.mkdir()
                    (failed_hook / 'fault.conf').write_text('[Service]\nExecStart=\nExecStart=/usr/bin/false\nExecStop=\n')
                if args.nvidia_ordering:
                    copy(HERE / 'test-nvidia-ordering.sh', '/usr/bin/test-nvidia-ordering')
                    ordering_script = root / 'usr/bin/test-nvidia-ordering'
                    ordering_script.chmod(0o755)
                    (root / 'etc/systemd/system/nvidia-suspend.service').write_text('[Unit]\nDescription=Fixture NVIDIA suspend hook\nDefaultDependencies=no\nBefore=systemd-suspend.service\n[Service]\nType=oneshot\nExecStart=/usr/bin/test-nvidia-ordering pre\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\n')
                    (root / 'etc/systemd/system/nvidia-resume.service').write_text('[Unit]\nDescription=Fixture NVIDIA resume hook\nDefaultDependencies=no\nAfter=systemd-suspend.service\n[Service]\nType=oneshot\nExecStart=/usr/bin/test-nvidia-ordering post\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\n')
                    suspend_wants = root / 'etc/systemd/system/systemd-suspend.service.wants'
                    suspend_wants.mkdir()
                    (suspend_wants / 'nvidia-suspend.service').symlink_to('../nvidia-suspend.service')
                    (suspend_wants / 'nvidia-resume.service').symlink_to('../nvidia-resume.service')
                    suspend_requires = root / 'etc/systemd/system/nvidia-suspend.service.requires'
                    suspend_requires.mkdir()
                    (suspend_requires / 'vram-swap-ublk-suspend.service').symlink_to('../vram-swap-ublk-suspend.service')
                (root / 'etc/systemd/sleep.conf').write_text('[Sleep]\nAllowSuspend=yes\nAllowHibernation=no\nSuspendState=mem\nMemorySleepMode=deep\n')
            copy(args.target, '/usr/local/libexec/vram-swap/ublk-lzo-target', True)
            copy(HERE / 'vram-swap-ublk-sleep.sh', '/usr/local/libexec/vram-swap/vram-swap-ublk-sleep.sh')
            copy(HERE / 'test-ublk-systemd-check.sh', '/usr/bin/systemd-check')
            (root / 'usr/bin/systemd-check').chmod(0o755)
            units = root / 'etc/systemd/system'
            units.mkdir(parents=True, exist_ok=True)
            copy(HERE / 'vram-swap-ublk.service', '/etc/systemd/system/vram-swap-ublk.service')
            dropin = units / 'vram-swap-ublk.service.d'
            dropin.mkdir()
            # Console transport replaces journald only; readiness and lifecycle use the staged unit.
            (dropin / 'console.conf').write_text('[Service]\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\n')
            if args.nvidia_ordering:
                (dropin / 'nvidia-ordering.conf').write_text('[Service]\nExecStartPre=/usr/bin/test-nvidia-ordering main-check\n')
            (units / 'sysinit.target').write_text('[Unit]\nDescription=Minimal isolated sysinit\nDefaultDependencies=no\n')
            (units / 'basic.target').write_text('[Unit]\nDescription=Minimal isolated basic target\nDefaultDependencies=no\n')
            (units / 'shutdown.target').write_text('[Unit]\nDescription=Isolated shutdown\nDefaultDependencies=no\n')
            (units / 'vram-test.target').write_text('[Unit]\nDescription=VRAM integration test\nDefaultDependencies=no\nRequires=vram-test.service\nAfter=vram-test.service\n')
            (units / 'vram-test.service').write_text('[Unit]\nDescription=VRAM integration check\nDefaultDependencies=no\nWants=vram-swap-ublk.service\nAfter=vram-swap-ublk.service\n[Service]\nType=oneshot\nExecStart=/usr/bin/systemd-check\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\nTimeoutStartSec=infinity\n')
            (root / 'etc/os-release').write_text('NAME=VRAM-Test\nID=vram-test\n')
            (root / 'etc/machine-id').write_text('72963c91c526483ca438b042b265fabc\n')
            (root / 'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
            (root / 'etc/group').write_text('root:x:0:\n')
            def target_option(name, default=None):
                for index, value in enumerate(args.target_arg):
                    if value == name and index + 1 < len(args.target_arg):
                        return args.target_arg[index + 1]
                    if value.startswith(name + '='):
                        return value.partition('=')[2]
                return default
            if target_option('--device') != '0' or target_option('--backing') != '/mnt/back' or target_option('--size') != str(args.capacity):
                parser.error('--systemd-lifecycle requires device 0, backing /mnt/back and size matching capacity')
            payload = target_option('--gpu-payload-bytes', '2097152')
            if not payload.isdecimal():
                parser.error('invalid systemd fixture GPU payload')
            (root / 'etc/vram-swap-ublk.conf').write_text(f'VRAM_UBLK_DEVICE=0\nVRAM_SWAP_BYTES={args.capacity}\nVRAM_BACKING_DIRECTORY=/mnt/back\nVRAM_GPU_PAYLOAD_BYTES={payload}\n')
        if args.managed_swap_lifecycle:
            for utility in ('mkswap', 'swapon', 'swapoff'):
                copy(Path('/usr/bin') / utility, '/usr/bin/' + utility, True)
        if args.managed_swapoff_failure_once:
            copy(Path('/usr/bin/swapoff'), '/usr/bin/swapoff.real', True)
            fault = temp / 'swapoff-fault'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-swapoff-fault.c'), '-o', str(fault)])
            copy(fault, '/usr/bin/swapoff', True)
        if args.safe_stop_probe or args.safe_stop_target:
            probe = temp / 'try-stop-probe'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-try-stop.c'), '-luring', '-o', str(probe)])
            copy(probe, '/usr/bin/try-stop-probe', True)
            loop_module = subprocess.check_output(['/usr/bin/modinfo', '-k', args.kernel_release, '-F', 'filename', 'loop'], text=True).strip()
            with (root / 'modules/loop.ko').open('wb') as out:
                run(['/usr/bin/zstd', '-dc', loop_module], stdout=out)
        if args.readonly_probe:
            probe = temp / 'readonly-probe'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-readonly.c'), '-o', str(probe)])
            copy(probe, '/usr/bin/readonly-probe', True)
        if args.stop_failure_once or args.stop_lost_reply:
            fault = temp / 'stop-fault.so'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                 str(HERE / 'test-ublk-stop-fault.c'), '-pthread', '-ldl', '-o', str(fault)])
            copy(fault, '/usr/lib/stop-fault.so', True)
        if args.start_params_failure:
            fault = temp / 'start-fault.so'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                 '-isystem', str(UBLK_SRC / 'include'), '-isystem', str(UBLK_SRC),
                 str(HERE / 'test-ublk-start-fault.c'), '-ldl', '-o', str(fault)])
            copy(fault, '/usr/lib/start-fault.so', True)
        if args.nvkvm_module:
            copy(args.nvkvm_module, '/modules/nvkvm.ko')
        copy(Path('/usr/bin/strace'), '/usr/bin/strace', True)
        copy(Path('/usr/bin/mkfs.ext4'), '/usr/bin/mkfs.ext4', True)
        if args.backing_fs == 'btrfs':
            copy(Path('/usr/bin/mkfs.btrfs'), '/usr/bin/mkfs.btrfs', True)
        for entry in args.include:
            host, sep, guest = entry.partition(':')
            if not sep or not guest.startswith('/'):
                parser.error('--include requires HOST:/absolute/guest/path')
            copy(Path(host), guest, os.access(host, os.X_OK))
        probe = temp / 'guest-check'
        run(['gcc', '-O2', '-Wall', '-Wextra', str(HERE / 'ublk-real-guest-check.c'), '-o', str(probe)])
        copy(probe, '/usr/bin/guest-check', True)
        if args.require_ready_notification:
            notify = temp / 'notify-guest'
            run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-notify-guest.c'), '-o', str(notify)])
            copy(notify, '/usr/bin/notify-guest', True)
        test = temp / 'test-ublk-real-io'
        run(['gcc', '-O2', '-Wall', '-Wextra', '-pthread', str(HERE / 'test-ublk-real-io.c'), '-o', str(test)])
        copy(test, '/usr/bin/test-ublk-real-io', True)
        if args.stop_with_held_io:
            held = temp / 'held-read'
            run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-held-read.c'), '-o', str(held)])
            copy(held, '/usr/bin/held-read', True)
        if args.fill_backing_filesystem:
            watcher = temp / 'guest-watch'
            run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-guest-watch.c'), '-o', str(watcher)])
            copy(watcher, '/usr/bin/guest-watch', True)
        if args.swap_pressure_mib:
            pressure = temp / 'swap-pressure'
            run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'test-ublk-swap-pressure.c'), '-o', str(pressure)])
            copy(pressure, '/usr/bin/swap-pressure', True)
        with (root / 'modules/ublk.ko').open('wb') as out:
            run(['/usr/bin/zstd', '-dc', module], stdout=out)
        (root / 'mode').write_text(args.mode + '\n')
        (root / 'cuda-bridge').write_text('1\n' if args.cuda_bridge_socket else '0\n')
        (root / 'native-cuda').write_text('1\n' if args.nvkvm_module else '0\n')
        (root / 'backing-probe').write_text('1\n' if args.backing_probe else '0\n')
        (root / 'cuda-fixture-init-failure').write_text('1\n' if args.cuda_fixture_init_failure else '0\n')
        (root / 'cuda-fixture-low-free').write_text('1\n' if auto_low_free else '0\n')
        (root / 'low-free-after-stop').write_text('1\n' if args.low_free_after_stop else '0\n')
        (root / 'stop-with-held-io').write_text('1\n' if args.stop_with_held_io else '0\n')
        (root / 'stop-failure-once').write_text('1\n' if args.stop_failure_once else '0\n')
        (root / 'stop-lost-reply').write_text('1\n' if args.stop_lost_reply else '0\n')
        (root / 'start-params-failure').write_text('1\n' if args.start_params_failure else '0\n')
        (root / 'readonly-probe').write_text('1\n' if args.readonly_probe else '0\n')
        (root / 'safe-stop-probe').write_text('1\n' if args.safe_stop_probe else '0\n')
        (root / 'safe-stop-target').write_text('1\n' if args.safe_stop_target else '0\n')
        (root / 'trace-io').write_text('1\n' if args.require_backing_io or args.swap_pressure_mib else '0\n')
        (root / 'capacity').write_text(str(args.capacity) + '\n')
        (root / 'require-ready-notification').write_text('1\n' if args.require_ready_notification else '0\n')
        (root / 'ready-notification-failure').write_text('1\n' if args.ready_notification_failure else '0\n')
        (root / 'managed-swap-lifecycle').write_text('1\n' if args.managed_swap_lifecycle else '0\n')
        (root / 'managed-swapoff-failure-once').write_text('1\n' if args.managed_swapoff_failure_once else '0\n')
        (root / 'systemd-lifecycle').write_text('1\n' if args.systemd_lifecycle else '0\n')
        (root / 'kernel-suspend').write_text('1\n' if args.kernel_suspend else '0\n')
        (root / 'sleep-hook-failure').write_text('1\n' if args.sleep_hook_failure else '0\n')
        (root / 'nvidia-ordering').write_text('1\n' if args.nvidia_ordering else '0\n')
        (root / 'swap-pressure-mib').write_text(str(args.swap_pressure_mib) + '\n')
        (root / 'swap-cycles').write_text(str(args.swap_cycles) + '\n')
        (root / 'drain-before-pageout').write_text('1\n' if args.drain_before_pageout else '0\n')
        (root / 'drain-with-swapped-pages').write_text('1\n' if args.drain_with_swapped_pages else '0\n')
        (root / 'fill-backing-filesystem').write_text('1\n' if args.fill_backing_filesystem else '0\n')
        (root / 'reclaim-filler-mib').write_text(str(args.reclaim_filler_mib) + '\n')
        (root / 'reclaim-pressure-mib').write_text(str(args.reclaim_pressure_mib) + '\n')
        (root / 'backing-fs').write_text(args.backing_fs + '\n')
        (root / 'btrfs-sectorsize').write_text(str(args.btrfs_sectorsize) + '\n')
        (root / 'guard-message').write_text(args.guard_message + '\n')
        (root / 'target-command').write_text('exec ' + shlex.join(['/usr/bin/target'] + args.target_arg) + '\n')
        listing = subprocess.check_output(['/usr/bin/find', '.', '-print0'], cwd=root)
        archive = subprocess.check_output(['/usr/bin/cpio', '--null', '-o', '-H', 'newc', '--quiet'], input=listing, cwd=root)
        initrd = temp / 'initramfs.gz'
        with gzip.open(initrd, 'wb', compresslevel=1) as f:
            f.write(archive)
        disk = temp / 'backing.raw'
        with disk.open('wb') as f:
            f.truncate(disk_bytes)
        cmdline = 'console=ttyS0 quiet panic=1 zswap.enabled=0'
        if args.mode != 'no-marker':
            cmdline += ' vram_isolated_guest=1'
        if args.mode != 'positive':
            cmdline += ' integration_negative=1'
        if args.nvidia_ordering:
            cmdline += ' integration_nvidia_ordering=1'
        cpu = 'host,-hypervisor' if args.mode == 'no-hypervisor' else 'host'
        # NVIDIA userspace + its compressed initrd do not fit the 512 MiB mock VM.
        command = [str(args.qemu), '-enable-kvm', '-cpu', cpu, '-m', '2048' if args.nvkvm_module else '512', '-smp', '2',
                   '-nodefaults', '-nographic', '-serial', 'stdio', '-no-reboot', '-net', 'none',
                   '-kernel', str(args.kernel), '-initrd', str(initrd), '-append', cmdline,
                   '-drive', f'file={disk},format=raw,if=virtio,cache=none']
        if args.mode == 'other-dmi':
            command += ['-smbios', 'type=1,manufacturer=ExampleCorp,product=ExampleMachine']
        if args.cuda_bridge_socket:
            command += ['-device', 'virtio-serial-pci', '-chardev',
                        'socket,id=cudabridge,path=' + str(args.cuda_bridge_socket),
                        '-device', 'virtserialport,chardev=cudabridge,name=vram.cuda']
        if args.nvkvm_module:
            command += ['-device', 'virtio-nvgpu-pci-non-transitional,graphics=off']
        wake_result = {}
        if args.kernel_suspend:
            qmp_path = temp / 'qmp.sock'
            command += ['-name', temp.name, '-qmp', f'unix:{qmp_path},server=on,wait=off']
        try:
            if not args.kernel_suspend:
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
            else:
                spec = importlib.util.spec_from_file_location('vram_qmp_wakeup', HERE / 'test-ublk-qmp-wakeup.py')
                qmp = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(qmp)
                cancel = threading.Event()
                def wake_guest():
                    try:
                        wake_result['result'] = qmp.wake_vm(qmp_path, temp.name, args.timeout, cancel)
                    except Exception as error:
                        wake_result['error'] = str(error)
                        print('VM_QMP_WAKEUP_ERROR ' + str(error), flush=True)
                with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
                    waker = threading.Thread(target=wake_guest)
                    waker.start()
                    try:
                        stdout, _ = process.communicate(timeout=args.timeout)
                        result = subprocess.CompletedProcess(command, process.returncode, stdout)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        stdout, _ = process.communicate()
                        raise subprocess.TimeoutExpired(command, args.timeout, output=stdout)
                    finally:
                        cancel.set()
                        waker.join(timeout=2)
        except subprocess.TimeoutExpired as exc:
            print((exc.stdout or b'').decode(errors='replace'))
            raise SystemExit('INCOMPLETE: VM timeout')
        output = result.stdout.decode(errors='replace')
        print(output, end='')
        if re.search(r'ordering cycle|dependency cycle|found ordering cycle', output, re.I):
            raise SystemExit('INCOMPLETE: systemd reported an ordering cycle in the NVIDIA fixture')
        if not (args.require_backing_io or args.swap_pressure_mib) and any(
            marker in output for marker in ('UBLK_BACKING_IO op=', 'UBLK_LZO_IO op=')
        ):
            raise SystemExit('INCOMPLETE: per-page tracing was unexpectedly enabled')
        fixture_present = 'CUDA_FIXTURE_NOT_REAL' in output
        bridge_present = 'CUDA_BRIDGE_CLIENT remote=1 staging=guest_not_cuda_pinned' in output
        if bridge_present != bool(args.cuda_bridge_socket):
            raise SystemExit('INCOMPLETE: CUDA bridge identity mismatch')
        if args.nvkvm_module and 'INTEGRATION_NATIVE_CUDA_MODULE_READY' not in output.splitlines():
            raise SystemExit('INCOMPLETE: native guest CUDA module not confirmed')
        if args.cuda_bridge_low_free and 'CUDA_BRIDGE_PRESSURE synthetic=1 ' not in output:
            raise SystemExit('INCOMPLETE: synthetic bridge pressure was not identified')
        if args.low_free_after_stop:
            positions = [output.find(marker) for marker in ('UBLK_STOP_RETAINED result=-16 ', 'INTEGRATION_RETAINED_STOP_PRESSURE_BEGIN', 'UBLK_AUTO_DRAIN reason=low_free', 'UBLK_DRAIN_OK ', 'INTEGRATION_RETAINED_DATA_PASS')]
            if min(positions) < 0 or positions != sorted(positions):
                raise SystemExit('INCOMPLETE: automatic retirement was not proven during retained shutdown')
        if args.cuda_native_low_free and 'UBLK_TEST_PRESSURE synthetic=1 ' not in output:
            raise SystemExit('INCOMPLETE: synthetic native pressure was not identified')
        if args.cuda_bridge_socket and args.swap_pressure_mib and not args.drain_before_pageout and not auto_low_free:
            phase = output.partition('INTEGRATION_PAGEOUT_BEGIN')[2].partition('INTEGRATION_PAGEOUT_END')[0]
            if not all('CUDA_BRIDGE_COPY op=' + op in phase for op in ('write', 'read')):
                raise SystemExit('INCOMPLETE: CUDA bridge did not prove both directions during swap workload')
        if fixture_present != args.expect_cuda_fixture:
            raise SystemExit('INCOMPLETE: CUDA fixture identity did not match explicit expectation')
        expected = 'INTEGRATION_NOTIFY_FAILURE_PASS' if args.ready_notification_failure else 'INTEGRATION_SAFE_STOP_TARGET_PASS' if args.safe_stop_target else 'INTEGRATION_SAFE_STOP_PROBE_PASS' if args.safe_stop_probe else 'INTEGRATION_START_PARAMS_REFUSAL_PASS' if args.start_params_failure else 'INTEGRATION_HELD_IO_STOP_PASS' if args.stop_with_held_io else 'INTEGRATION_CUDA_INIT_REFUSAL_PASS' if args.cuda_fixture_init_failure else 'INTEGRATION_BACKING_PROBE_PASS' if args.backing_probe else 'INTEGRATION_PASS' if args.mode == 'positive' else 'NEGATIVE_PASS'
        if args.managed_swap_lifecycle:
            expected = 'INTEGRATION_MANAGED_SWAP_PASS'
        if args.systemd_lifecycle:
            expected = 'INTEGRATION_SYSTEMD_LIFECYCLE_PASS'
        if args.sleep_hook_failure:
            expected = 'INTEGRATION_SLEEP_FAILURE_BLOCKED_PASS'
        if result.returncode or expected not in output.splitlines() and not any(line.startswith(expected + ' ') for line in output.splitlines()) or 'INTEGRATION_FAIL' in output:
            raise SystemExit('INCOMPLETE: guest did not prove success')
        if args.require_ready_notification and 'INTEGRATION_NOTIFY_READY_PASS' not in output.splitlines():
            raise SystemExit('INCOMPLETE: target readiness datagram was not verified')
        if args.systemd_lifecycle:
            if args.sleep_hook_failure:
                if not all(marker in output.splitlines() for marker in ('INTEGRATION_SLEEP_FAILURE_BLOCKED_PASS', 'INTEGRATION_SLEEP_ENTRY_COUNT_UNCHANGED')) or 'PASS: verified all ' not in output or not re.search(r'MAPPING_SWAPPED_PAGES=[1-9][0-9]*', output):
                    raise SystemExit('INCOMPLETE: failed pre-sleep hook did not block suspension and preserve data')
                if args.nvidia_ordering:
                    if 'INTEGRATION_NVIDIA_FAILURE_BLOCKED_PASS' not in output.splitlines() or 'INTEGRATION_NVIDIA_PRE_ENTERED' in output.splitlines():
                        raise SystemExit('INCOMPLETE: failed evacuation did not block the NVIDIA action')
                    print('VM_NVIDIA_FAILURE_BLOCKED_VERIFIED')
                print('VM_SLEEP_FAILURE_BLOCKED_VERIFIED')
                return
            if args.kernel_suspend:
                if wake_result.get('result') != {'suspended': True, 'woken': True} or not all(marker in output.splitlines() for marker in ('INTEGRATION_KERNEL_SUSPEND_BEGIN', 'INTEGRATION_KERNEL_RESUMED', 'INTEGRATION_NVKVM_QUIESCED', 'INTEGRATION_NVKVM_RELOADED')):
                    raise SystemExit('INCOMPLETE: guest kernel suspend/wakeup not proven: ' + str(wake_result))
                print('VM_KERNEL_SUSPEND_WAKEUP_VERIFIED host_suspend=0')
            if args.nvidia_ordering:
                lines = output.splitlines()
                pre_marker = 'INTEGRATION_NVIDIA_PRE_EVACUATED'
                resume_marker = 'INTEGRATION_NVIDIA_RESUME_READY'
                final_marker = 'INTEGRATION_SYSTEMD_LIFECYCLE_PASS'
                pre = lines.index(pre_marker) if lines.count(pre_marker) == 1 else -1
                resumed = lines.index(resume_marker) if lines.count(resume_marker) == 1 else -1
                ready_lines = [index for index, line in enumerate(lines) if line.startswith('UBLK_VMM_READY ')]
                second_ready = next((index for index in ready_lines if index > resumed), -1)
                final_pass = lines.index(final_marker) if lines.count(final_marker) == 1 else -1
                if min(pre, resumed, second_ready, final_pass) < 0 or not pre < resumed < second_ready < final_pass:
                    raise SystemExit('INCOMPLETE: NVIDIA PRE/RESUME/second-start ordering was not proven')
                print('VM_NVIDIA_ORDERING_VERIFIED')
            if not all(marker in output for marker in ('INTEGRATION_SYSTEMD_SWAPPED', 'INTEGRATION_SYSTEMD_EVACUATED', 'MAPPING_SWAPPED_PAGES=', 'PASS: verified all ')):
                raise SystemExit('INCOMPLETE: systemd lifecycle did not prove live swap evacuation and data')
            print('VM_SYSTEMD_LIFECYCLE_VERIFIED cuda=native_guest_nvkvm_experimental')
            return
        if args.managed_swap_lifecycle:
            ordered = [output.find(marker) for marker in ('UBLK_MANAGED_SWAP_ACTIVE', 'INTEGRATION_NOTIFY_READY_PASS', 'INTEGRATION_MANAGED_SWAPPED', 'UBLK_MANAGED_SWAP_OFF', 'UBLK_STOP_DONE', 'INTEGRATION_MANAGED_SWAP_PASS')]
            if min(ordered) < 0 or ordered != sorted(ordered) or not re.search(r'MAPPING_SWAPPED_PAGES=[1-9][0-9]*', output) or 'PASS: verified all ' not in output:
                raise SystemExit('INCOMPLETE: managed swap lifecycle did not prove activation, evacuation and retained data')
            if args.managed_swapoff_failure_once:
                retained = [output.find(marker) for marker in ('UBLK_SWAPOFF_FAULT synthetic=1 call=1', 'UBLK_MANAGED_SWAPOFF_FAILED', 'UBLK_STOP_RETAINED result=-16 resources_retained=1', 'INTEGRATION_MANAGED_RETAINED_DATA_PASS', 'UBLK_MANAGED_SWAP_OFF', 'UBLK_STOP_DONE')]
                if min(retained) < 0 or retained != sorted(retained) or output.count('UBLK_SWAPOFF_FAULT synthetic=1 call=1') != 1:
                    raise SystemExit('INCOMPLETE: failed managed swapoff did not retain data until successful retry')
                print('VM_MANAGED_SWAPOFF_RETRY_VERIFIED')
            print('VM_MANAGED_SWAP_VERIFIED')
            return
        if args.ready_notification_failure:
            if 'ublk readiness notification:' not in output or 'UBLK_STOP_DONE' not in output.splitlines() or 'UBLK_LZO_READY ' in output:
                raise SystemExit('INCOMPLETE: notification failure did not prove safe cleanup')
            print('VM_NOTIFY_FAILURE_VERIFIED')
            return
        if args.safe_stop_target:
            if 'UBLK_SAFE_STOP_ENABLED kernel_openers=1 no_fallback=1' not in output or output.count('INTEGRATION_SAFE_STOP_BUSY_CONFIRMED') != 2 or 'INTEGRATION_SAFE_STOP_UNRELATED_SWAP_RETAINED' not in output.splitlines() or 'UBLK_STOP_DONE' not in output:
                raise SystemExit('INCOMPLETE: full safe-stop target integration not proven')
            print('VM_SAFE_STOP_TARGET_VERIFIED')
            return
        if args.safe_stop_probe:
            if output.count('UBLK_TRY_STOP_PROBE expected=busy result=-16') != 2 or 'UBLK_TRY_STOP_PROBE expected=stopped result=0' not in output or 'INTEGRATION_SAFE_STOP_UNRELATED_SWAP_RETAINED' not in output.splitlines():
                raise SystemExit('INCOMPLETE: kernel safe stop did not cover direct, loop and unrelated swap')
            print('VM_SAFE_STOP_PROBE_VERIFIED')
            return
        if args.readonly_probe and not all(marker in output.splitlines() for marker in (
            'INTEGRATION_READONLY_SWAPPED_PAGES', 'INTEGRATION_READONLY_ACTIVATION_REFUSED',
        )):
            raise SystemExit('INCOMPLETE: read-only gate experiment was not verified')
        if args.start_params_failure:
            if 'UBLK_START_FAULT synthetic=1 stage=set_params' not in output or 'UBLK_LZO_READY ' in output:
                raise SystemExit('INCOMPLETE: parameter failure was not confirmed before readiness')
            print('VM_START_PARAMS_REFUSAL_VERIFIED')
            return
        if args.stop_failure_once:
            markers = ['UBLK_STOP_FAULT synthetic=1 call=1',
                       'UBLK_STOP_RETAINED result=-5 resources_retained=1',
                       'INTEGRATION_STOP_RETAINED_IO_BEGIN', 'INTEGRATION_STOP_RETAINED_IO_PASS',
                       'UBLK_STOP_FAULT_RETRY call=2', 'UBLK_STOP_DONE', 'INTEGRATION_SHUTDOWN_PASS']
            positions = [output.find(marker) for marker in markers]
            if min(positions) < 0 or positions != sorted(positions) or output.count('UBLK_STOP_FAULT synthetic=1 call=1') != 1:
                raise SystemExit('INCOMPLETE: failed STOP did not retain working target until confirmed retry')
        if args.stop_lost_reply:
            markers = ['UBLK_STOP_REPLY_LOST synthetic=1 kernel_result=0',
                       'UBLK_STOP_RETAINED result=-5 resources_retained=1',
                       'INTEGRATION_STOP_REPLY_LOST_RETAINED_PASS',
                       'UBLK_STOP_FAULT_RETRY call=2', 'UBLK_STOP_DONE', 'INTEGRATION_SHUTDOWN_PASS']
            positions = [output.find(marker) for marker in markers]
            if min(positions) < 0 or positions != sorted(positions):
                raise SystemExit('INCOMPLETE: lost STOP reply recovery was not verified')
        if args.stop_with_held_io:
            markers = ['UBLK_STOP_BEGIN', 'INTEGRATION_HELD_RELEASE_BEGIN', 'UBLK_STOP_DONE']
            positions = [output.find(marker) for marker in markers]
            if min(positions) < 0 or positions != sorted(positions) or 'HELD_READ_PASS' not in output.splitlines() or any(marker in output for marker in ('UBLK_STORE_ERROR', 'UBLK_REQUEST_REJECTED', 'cannot drain')):
                raise SystemExit('INCOMPLETE: held I/O shutdown ordering or completion failed')
            print('VM_HELD_IO_STOP_VERIFIED cuda=fixture_not_hardware')
            return
        if args.cuda_fixture_init_failure:
            print('VM_CUDA_INIT_REFUSAL_VERIFIED cuda=fixture_not_hardware')
            return
        if args.backing_probe:
            print('VM_BACKING_PROBE_VERIFIED ublk_loaded=0 swap_activated=0')
            return
        if args.mode == 'positive' and 'INTEGRATION_SHUTDOWN_PASS' not in output.splitlines():
            raise SystemExit('INCOMPLETE: shutdown was not verified')
        if args.mode == 'positive' and (
            'INTEGRATION_ACTIVE_SWAP_SIGNAL_PASS' not in output.splitlines() or
            output.count('UBLK_STOP_RETAINED result=-16 resources_retained=1' if '--kernel-safe-stop' in args.target_arg else 'UBLK_SHUTDOWN_DEFERRED swap active or state unavailable') < 2
        ):
            raise SystemExit('INCOMPLETE: active swap did not defer both shutdown signals')
        if args.mode == 'positive' and 'INTEGRATION_SWAP_ALIAS_PASS' not in output.splitlines():
            raise SystemExit('INCOMPLETE: swap alias was not verified')
        if args.fill_backing_filesystem and args.mode == 'positive' and 'INTEGRATION_BACKING_ENOSPC_REACHED' not in output.splitlines():
            raise SystemExit('INCOMPLETE: filesystem exhaustion was not reached')
        if args.expect_emergency_release and (
            'UBLK_DISK_RESERVE_READY bytes=67108864' not in output or
            output.count('UBLK_DISK_RESERVE_RELEASED') != 1 or
            'UBLK_DISK_RESERVE_FAILED' in output
        ):
            raise SystemExit('INCOMPLETE: one-time emergency reserve release was not verified')
        if args.reclaim_filler_mib and args.mode == 'positive' and (
            'INTEGRATION_FILLER_RECLAIMED_MIB=' + str(args.reclaim_filler_mib) not in output.splitlines()
        ):
            raise SystemExit('INCOMPLETE: requested filler space was not reclaimed')
        if args.mode == 'positive' and any(marker in output for marker in (
            'UBLK_STORE_ERROR', 'UBLK_REQUEST_REJECTED', 'Out of memory:',
            'ublk queue exited unexpectedly', 'cannot drain',
        )):
            raise SystemExit('INCOMPLETE: target or guest reported an I/O or lifecycle failure')
        if args.require_backing_io and args.mode == 'positive':
            if not auto_low_free and not all('UBLK_LZO_IO op=' + op in output for op in ('write', 'read')):
                raise SystemExit('INCOMPLETE: LZO writes and reads were not both exercised')
            written = set(re.findall(r'^UBLK_BACKING_IO op=write page=(\d+)\r?$', output, re.M))
            read = set(re.findall(r'^UBLK_BACKING_IO op=read page=(\d+)\r?$', output, re.M))
            if not written or not written.issubset(read):
                raise SystemExit('INCOMPLETE: backing writes were absent or not all read back')
            print('BACKING_VERIFIED pages=' + str(len(written)))
        if args.swap_pressure_mib and args.mode == 'positive':
            if auto_low_free and (
                'UBLK_AUTO_DRAIN reason=low_free' not in output or
                len(re.findall(r'^UBLK_DRAIN_OK seq=\d+ errno=0\r?$', output, re.M)) != 1 or
                'UBLK_DRAIN_FAILED ' in output
            ):
                raise SystemExit('INCOMPLETE: automatic low-free retirement was not verified')
            if auto_low_free and (
                'INTEGRATION_RETAINED_DATA_PASS' not in output.splitlines() or
                output.find('UBLK_DRAIN_OK ') > output.find('INTEGRATION_RETAINED_DATA_BEGIN')
            ):
                raise SystemExit('INCOMPLETE: prior device data was not verified after retirement')
            if (args.drain_before_pageout or args.drain_with_swapped_pages) and (
                'UBLK_DRAIN_OK ' not in output or 'UBLK_DRAIN_FAILED ' in output
            ):
                raise SystemExit('INCOMPLETE: requested pool drain did not succeed')
            if args.drain_before_pageout or args.drain_with_swapped_pages:
                requests = re.findall(r'^UBLK_DRAIN_REQUESTED seq=(\d+)\r?$', output, re.M)
                successes = re.findall(r'^UBLK_DRAIN_OK seq=(\d+) errno=0\r?$', output, re.M)
                repeated = re.findall(r'^UBLK_DRAIN_STATUS state=3 seq=(\d+) errno=0\r?$', output, re.M)
                if len(requests) != 1 or successes != requests or repeated != requests:
                    raise SystemExit('INCOMPLETE: repeated drain was not idempotent')
            if args.drain_with_swapped_pages:
                begin = output.find('INTEGRATION_SWAPPED_DRAIN_BEGIN')
                done = output.find('INTEGRATION_DRAIN_CONFIRMED seq=')
                digest = output.find('INTEGRATION_RETAINED_DIGEST_PASS ')
                end = output.find('INTEGRATION_SWAPPED_DRAIN_END')
                if min(begin, done, digest, end) < 0 or not begin < done < digest < end:
                    raise SystemExit('INCOMPLETE: drain was not completed while swapped mapping was held')
                confirmed = re.findall(r'^INTEGRATION_DRAIN_CONFIRMED seq=(\d+)\r?$', output, re.M)
                if confirmed != successes:
                    raise SystemExit('INCOMPLETE: guest confirmation did not match completed drain')
                if not re.search(r'^INTEGRATION_RETAINED_DIGEST_PASS UBLK_DEVICE_DIGEST bytes=' + str(args.capacity) + r' fnv1a64=[0-9a-f]{16}\r?$', output, re.M):
                    raise SystemExit('INCOMPLETE: full device contents were not compared across retirement')
            if output.splitlines().count('INTEGRATION_PENDING_SWAP_SIGNAL_PASS') != args.swap_cycles:
                raise SystemExit('INCOMPLETE: pending-page signal checks were not all completed')
            if output.count('UBLK_STOP_RETAINED result=-16 resources_retained=1' if '--kernel-safe-stop' in args.target_arg else 'UBLK_SHUTDOWN_DEFERRED swap active or state unavailable') < 3 + 2 * args.swap_cycles:
                raise SystemExit('INCOMPLETE: shutdown deferral evidence missing')
            cycles = [int(n) for n in re.findall(r'^INTEGRATION_SWAP_CYCLE_PASS cycle=(\d+)\r?$', output, re.M)]
            if cycles != list(range(1, args.swap_cycles + 1)):
                raise SystemExit('INCOMPLETE: missing swap cycles')
            swapins = [int(n) for n in re.findall(r'^pswpin (\d+)\r?$', output, re.M)]
            if len(swapins) != 2 or swapins[1] <= swapins[0]:
                raise SystemExit('INCOMPLETE: no positive swap-in counter delta')
            phase = output.split('INTEGRATION_PAGEOUT_BEGIN', 1)[-1].split('INTEGRATION_PAGEOUT_END', 1)[0]
            if 'INTEGRATION_PAGEOUT_BEGIN' not in output or not all(
                'UBLK_BACKING_IO op=' + op in phase for op in ('write', 'read')
            ):
                raise SystemExit('INCOMPLETE: pageout did not prove backing writes and reads')
        print('VM_VERIFIED mode=' + args.mode + (' cuda=fixture_not_hardware' if args.expect_cuda_fixture else ' cuda=remote_backend_requires_server_evidence' if args.cuda_bridge_socket else ' cuda=native_guest_nvkvm_experimental' if args.nvkvm_module else ''))


if __name__ == '__main__':
    main()
