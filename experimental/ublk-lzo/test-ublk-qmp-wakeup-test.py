#!/usr/bin/env python3
"""Socket-only tests for test-ublk-qmp-wakeup.py; no QEMU or GPU required."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import time
from unittest import mock
from types import SimpleNamespace
import stat


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("qmp_wakeup", HERE / "test-ublk-qmp-wakeup.py")
WAKEUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WAKEUP)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(case, succeeds=False):
    with tempfile.TemporaryDirectory(prefix="qmp-wakeup-") as tmp:
        directory = Path(tmp)
        directory.chmod(0o700)
        path = str(directory / "qmp.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        listener.listen(1)
        cancel = threading.Event()
        commands = []
        failures = []

        def server():
            conn = None
            try:
                conn, _ = listener.accept()
                conn.settimeout(2)
                conn.sendall(b'{"QMP":{"version":{},"capabilities":[]}}\r\n')
                reader = conn.makefile("rb")

                def command(expected):
                    message = json.loads(reader.readline())
                    commands.append(message["execute"])
                    check(message == {"execute": expected, "id": len(commands)}, f"unexpected command: {message}")
                    return message

                def reply(ident, result):
                    conn.sendall((json.dumps({"return": result, "id": ident}) + "\r\n").encode())

                command("qmp_capabilities")
                reply(1, {})
                command("query-name")
                reply(2, {"name": "vm-test" if case != "wrong-name" else "other"})
                if case == "wrong-name":
                    return
                command("query-current-machine")
                reply(3, {"wakeup-suspend-support": case != "unsupported"})
                if case == "unsupported":
                    return
                conn.sendall(b'{"event":"SUSPEND"}\r\n')
                command("query-status")
                reply(4, {"status": "suspended"})
                command("system_wakeup")
                if case == "disconnect":
                    return
                if case == "wake-error":
                    conn.sendall(b'{"error":{"class":"GenericError","desc":"no"},"id":5}\r\n')
                    return
                if case == "success":
                    conn.sendall(b'{"event":"WAKEUP"}\r\n')
                    time.sleep(0.03)
                reply(5, {})
                if case == "timeout":
                    time.sleep(0.5)
                    return
                if case == "cancel":
                    time.sleep(0.5)
                    return
                command("query-status")
                reply(6, {"status": "running"})
            except BaseException as exc:
                failures.append(exc)
            finally:
                if conn is not None:
                    conn.close()

        thread = threading.Thread(target=server, daemon=False)
        thread.start()
        if case == "cancel":
            threading.Timer(0.08, cancel.set).start()
        try:
            try:
                result = WAKEUP.wake_vm(path, "vm-test", 0.18 if case == "timeout" else 2, cancel)
            except (TimeoutError, InterruptedError, EOFError, RuntimeError, ValueError) as exc:
                result = exc
            check((isinstance(result, dict)) == succeeds, f"{case}: unexpected result {result!r}")
            if succeeds:
                check(result == {"suspended": True, "woken": True}, "success result mismatch")
                check(commands == ["qmp_capabilities", "query-name", "query-current-machine",
                                   "query-status", "system_wakeup", "query-status"], "success command sequence mismatch")
            if case == "wrong-name":
                check(commands == ["qmp_capabilities", "query-name"], "wrong name sent further commands")
            if case == "unsupported":
                check(commands == ["qmp_capabilities", "query-name", "query-current-machine"],
                      "unsupported machine sent wake command")
            if case == "wake-error":
                check("query-status" not in commands[4:], "wake error sent final status query")
        finally:
            listener.close()
            thread.join(2)
            check(not thread.is_alive(), f"{case}: server thread leaked")
            check(not failures, f"{case}: server failure {failures!r}")


def main():
    run_case("success", succeeds=True)
    for case in ("wrong-name", "unsupported", "wake-error", "disconnect", "timeout", "cancel"):
        run_case(case)
    with tempfile.TemporaryDirectory(prefix="qmp-path-") as tmp:
        path = Path(tmp) / 'not-a-socket'
        path.touch()
        for socket_stat in (SimpleNamespace(st_uid=os.getuid(), st_mode=stat.S_IFREG),
                            SimpleNamespace(st_uid=os.getuid() + 1, st_mode=stat.S_IFSOCK)):
            with mock.patch.object(WAKEUP.os, 'lstat', return_value=socket_stat):
                try:
                    WAKEUP.wake_vm(path, 'vm-test', 0.2, threading.Event())
                except PermissionError:
                    pass
                else:
                    raise AssertionError('invalid socket identity accepted')
    print("QMP_WAKEUP_PASS cases=9 QEMU=0 GPU=0")


if __name__ == "__main__":
    main()
