#!/usr/bin/env python3
"""CPU-only lifecycle tests for host_pageout_probe; never runs system commands."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('host_pageout_probe', HERE / 'host_pageout_probe.py')
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class Target:
    pid = 4242

    def send_signal(self, sig):
        self.events.append('target-signal')

    def __init__(self, events):
        self.events = events


def exercise(*, fail_pop=False, fail_cgroup=False, fail_cleanup=False, no_faults=False, no_device_reads=False):
    events, commands = [], []
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / 'cgroup.subtree_control').write_text('memory\n')
        evidence = root / 'evidence'
        evidence.mkdir()
        real_popen = subprocess.Popen
        helper = probe.Path(__file__).with_name('test-ublk-host-pageout')
        real_is_file = Path.is_file
        worker_ref = []

        def run(command, **kwargs):
            commands.append(list(command))
            events.append(Path(command[0]).name)
            return subprocess.CompletedProcess(command, 0)

        def mkdir(path, *args, **kwargs):
            if fail_cgroup and Path(path).name.startswith('vram-pageout-test-'):
                raise OSError('injected cgroup setup failure')
            return real_mkdir(path, *args, **kwargs)

        real_mkdir = Path.mkdir

        def popen(command, **kwargs):
            events.append('popen')
            if fail_pop:
                raise OSError('injected worker spawn failure')
            # This real Python child is deliberately inert: it prints protocol
            # markers and waits for the parent's R byte; it never execs helper.
            code = ("import sys; assert sys.stdin.buffer.readline() == b'GO\\n'; "
                    "print('PAGEOUT_READY pages=1 type=0 attributed=1', flush=True); "
                    "assert sys.stdin.buffer.read(1) == b'R'; print('PAGEOUT_READBACK_PASS', flush=True)")
            child = real_popen([sys.executable, '-u', '-c', code], **kwargs)
            worker_ref.append(child)
            return child

        def drain(marker):
            events.append('drain-ok')

        with mock.patch.object(probe, 'CGROUP_ROOT', root), \
             mock.patch.object(probe, 'available_bytes', return_value=8 * 1024**3), \
             mock.patch.object(probe, 'major_faults', side_effect=[0, 0 if no_faults else 4096]), \
             mock.patch.object(probe, 'device_read_bytes', side_effect=[0, 0 if no_device_reads else 4096]), \
             mock.patch.object(Path, 'is_file', lambda self: True if self == helper else real_is_file(self)), \
             mock.patch.object(subprocess, 'run', side_effect=run), \
             mock.patch.object(subprocess, 'Popen', side_effect=popen), \
             mock.patch.object(Path, 'mkdir', mkdir), \
             mock.patch.object(probe.signal, 'SIGUSR2', 12), \
             mock.patch.object(Target, 'send_signal', lambda self, sig: events.append('target-signal')):
            # Wrap the actual subtree-control open to keep the temporary file
            # readable, and make the cgroup cleanup failure independently injectable.
            original_rmdir = Path.rmdir

            def rmdir(path):
                if fail_cleanup and Path(path).name.startswith('vram-pageout-test-'):
                    raise OSError('injected cgroup cleanup failure')
                for child in Path(path).iterdir():
                    child.unlink()
                return original_rmdir(path)

            with mock.patch.object(Path, 'rmdir', rmdir):
                outcome = None
                try:
                    probe.run_pageout(Target(events), evidence, drain)
                except Exception as exc:
                    outcome = exc
        assert commands and commands[0][0].endswith('test-ublk-host-pageout')
        if not fail_cgroup:
            assert b'PAGEOUT_READBACK_PASS' in (evidence / 'pageout.log').read_bytes() if outcome is None else True
        if worker_ref and worker_ref[0].poll() is None:
            worker_ref[0].kill()
            worker_ref[0].wait()
        if not fail_cleanup:
            assert not list(root.glob('vram-pageout-test-*'))
        return events, commands, outcome, root


def successful_order():
    events, commands, outcome, root = exercise()
    assert outcome is None, outcome
    names = [Path(command[0]).name for command in commands]
    assert names[:3] == ['test-ublk-host-pageout', 'mkswap', 'swapon']
    assert names[-1] == 'swapoff'
    assert events.index('target-signal') < events.index('drain-ok') < events.index('swapoff')
    assert not list(root.glob('vram-pageout-test-*'))


def failures():
    events, commands, outcome, root = exercise(fail_pop=True)
    assert isinstance(outcome, OSError) and any(Path(c[0]).name == 'swapoff' for c in commands)
    assert commands[-1][0].endswith('swapoff')
    events, commands, outcome, root = exercise(no_device_reads=True)
    assert isinstance(outcome, RuntimeError) and 'device reads' in str(outcome)
    assert commands[-1][0].endswith('swapoff')
    events, commands, outcome, root = exercise(fail_cgroup=True)
    assert isinstance(outcome, OSError) and commands[-1][0].endswith('swapoff')
    assert events.index('popen') < events.index('swapoff')
    events, commands, outcome, root = exercise(fail_cleanup=True)
    assert isinstance(outcome, OSError) and commands[-1][0].endswith('swapoff')
    events, commands, outcome, root = exercise(no_faults=True)
    assert isinstance(outcome, RuntimeError) and 'major page faults' in str(outcome)
    assert commands[-1][0].endswith('swapoff')


successful_order()
failures()
print('HOST_PAGEOUT_PROBE_TEST_PASS cases=6 commands_mocked=1 real_child=1 GPU=0 cgroups=temporary swap=0')
