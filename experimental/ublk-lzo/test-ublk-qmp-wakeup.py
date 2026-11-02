#!/usr/bin/env python3
"""Wake a suspended QEMU VM through its private QMP Unix socket."""
import json
import os
import socket
import stat
import time


_LIMIT = 65536
_POLL = 0.25


def wake_vm(path, expected_name, timeout, cancel_event):
    deadline = time.monotonic() + timeout
    path = os.fspath(path)
    parent = os.path.dirname(path) or "."
    st = os.stat(parent)
    if st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o700:
        raise PermissionError("QMP socket directory must be owned by this UID and mode 0700")

    def remaining():
        if cancel_event.is_set():
            raise InterruptedError("QMP wakeup cancelled")
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("QMP wakeup deadline expired")
        return left

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    stream = bytearray()
    events = set()
    try:
        while True:
            left = remaining()
            try:
                socket_stat = os.lstat(path)
                if socket_stat.st_uid != os.getuid() or not stat.S_ISSOCK(socket_stat.st_mode):
                    raise PermissionError("QMP path must be a socket owned by this UID")
                sock.settimeout(min(_POLL, left))
                sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                cancel_event.wait(min(0.05, remaining()))
        sock.settimeout(min(_POLL, remaining()))

        def receive():
            while True:
                remaining()
                newline = stream.find(b"\n")
                if newline >= 0:
                    raw = bytes(stream[:newline])
                    del stream[:newline + 1]
                    try:
                        return json.loads(raw)
                    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                        raise ValueError("invalid QMP JSON") from exc
                if len(stream) >= _LIMIT:
                    raise ValueError("QMP line exceeds 64 KiB")
                try:
                    sock.settimeout(min(_POLL, remaining()))
                    part = sock.recv(min(4096, _LIMIT + 1 - len(stream)))
                except socket.timeout:
                    continue
                if not part:
                    raise EOFError("QMP socket closed")
                stream.extend(part)

        def send(command, ident):
            packet = json.dumps({"execute": command, "id": ident}, separators=(",", ":")).encode() + b"\r\n"
            view = memoryview(packet)
            while view:
                remaining()
                try:
                    sock.settimeout(min(_POLL, remaining()))
                    sent = sock.send(view)
                except socket.timeout:
                    continue
                if not sent:
                    raise EOFError("QMP socket closed while sending")
                view = view[sent:]

        def response(ident):
            while True:
                message = receive()
                if not isinstance(message, dict):
                    raise ValueError("invalid QMP message")
                if "event" in message:
                    if not isinstance(message["event"], str):
                        raise ValueError("invalid QMP event")
                    if message["event"] in ("SUSPEND", "WAKEUP"):
                        events.add(message["event"])
                    continue
                if message.get("id") != ident or ("return" in message) == ("error" in message):
                    raise ValueError("unexpected QMP response")
                if "error" in message:
                    raise RuntimeError("QMP command failed")
                return message["return"]

        greeting = receive()
        if not isinstance(greeting, dict) or not isinstance(greeting.get("QMP"), dict):
            raise ValueError("invalid QMP greeting")
        send("qmp_capabilities", 1)
        response(1)
        send("query-name", 2)
        name = response(2)
        if not isinstance(name, dict) or name.get("name") != expected_name:
            raise ValueError("unexpected QEMU name")
        send("query-current-machine", 3)
        machine = response(3)
        if not isinstance(machine, dict) or machine.get("wakeup-suspend-support") is not True:
            raise ValueError("QEMU does not support wakeup from suspend")
        while "SUSPEND" not in events:
            receive_event = receive()
            if not isinstance(receive_event, dict) or "event" not in receive_event:
                raise ValueError("unexpected QMP message while waiting for SUSPEND")
            if receive_event["event"] == "SUSPEND":
                events.add("SUSPEND")
        events.discard("WAKEUP")
        send("query-status", 4)
        status = response(4)
        if not isinstance(status, dict) or status.get("status") != "suspended":
            raise ValueError("QEMU is not suspended")
        send("system_wakeup", 5)
        response(5)
        while "WAKEUP" not in events:
            event = receive()
            if not isinstance(event, dict) or "event" not in event:
                raise ValueError("unexpected QMP message while waiting for WAKEUP")
            if event["event"] == "WAKEUP":
                events.add("WAKEUP")
        send("query-status", 6)
        status = response(6)
        if not isinstance(status, dict) or status.get("status") != "running":
            raise ValueError("QEMU is not running after wakeup")
        return {"suspended": True, "woken": True}
    finally:
        sock.close()
