#!/usr/bin/env python3
"""CPU-only client tests for malformed CUDA bridge replies."""
import ctypes
import importlib.util
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading

HERE = Path(__file__).resolve().parent
MESSAGE = struct.Struct("=6I5Q")
MAGIC = 0x43425231
VERSION = 1
INIT = 1
UNKNOWN = 999
TIMEOUT = 8


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        part = conn.recv(size - len(data))
        if not part:
            raise RuntimeError("client closed before full request")
        data.extend(part)
    return bytes(data)


def child_call(library):
    lib = ctypes.CDLL(str(library))
    init = lib.cuInit
    init.argtypes = [ctypes.c_uint]
    init.restype = ctypes.c_int
    first, second = init(0), init(0)
    if (first, second) != (UNKNOWN, UNKNOWN):
        raise AssertionError(f"cuInit results were {(first, second)}, expected (999, 999)")


def run_case(work, kind):
    path = work / f"{kind}.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.settimeout(TIMEOUT)
    server.bind(str(path))
    server.listen(2)
    errors = []

    def serve():
        try:
            conn, _ = server.accept()
            with conn:
                conn.settimeout(TIMEOUT)
                request = MESSAGE.unpack(recv_exact(conn, MESSAGE.size))
                magic, version, op, length, status, reserved, seq, ctx, a, b, n = request
                if (magic, version, op, length, status, reserved, seq, ctx, n) != (
                    MAGIC, VERSION, INIT, 0, 0, 0, 1, 0, 0
                ):
                    raise AssertionError(f"unexpected INIT request: {request}")
                reply_magic = magic if kind != "wrong-magic" else 0
                reply_seq = seq + 1 if kind == "wrong-sequence" else seq
                reply = MESSAGE.pack(reply_magic, version, op, 0, 0, 0,
                                     reply_seq, ctx, 0, 0, 0)
                conn.sendall(reply if kind != "truncated" else reply[:17])
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    env = os.environ.copy()
    env["CUDA_BRIDGE_SOCKET"] = str(path)
    env.pop("CUDA_BRIDGE_DEVICE", None)
    try:
        result = subprocess.run(
            [os.environ.get("PYTHON", "python3"), "-c",
             "import importlib.util, sys; "
             "s=importlib.util.spec_from_file_location('bridge_test', sys.argv[1]); "
             "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
             "m.child_call(sys.argv[2])",
             str(Path(__file__).resolve()), str(work / "libcuda-bridge.so.1")],
            env=env, capture_output=True, text=True, timeout=TIMEOUT,
        )
        thread.join(TIMEOUT)
        if thread.is_alive():
            raise RuntimeError(f"{kind}: server thread timed out")
        if errors:
            raise RuntimeError(f"{kind}: mock server failed: {errors[0]}") from errors[0]
        if result.returncode:
            raise RuntimeError(f"{kind}: child failed: {result.stderr[-1000:]}")
        server.settimeout(0.1)
        try:
            extra, _ = server.accept()
        except TimeoutError:
            pass
        else:
            extra.close()
            raise AssertionError(f"{kind}: shim reconnected after channel failure")
    finally:
        server.close()
        if thread.is_alive():
            thread.join(TIMEOUT)
        path.unlink(missing_ok=True)


def main():
    with tempfile.TemporaryDirectory(prefix="cuda-bridge-errors-") as temp:
        work = Path(temp)
        subprocess.run([
            "cc", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
            "-pthread", str(HERE / "test-libcuda-bridge.c"),
            "-o", str(work / "libcuda-bridge.so.1"),
        ], check=True, timeout=TIMEOUT, capture_output=True, text=True)
        for kind in ("wrong-magic", "wrong-sequence", "truncated"):
            run_case(work, kind)
    print("CUDA_BRIDGE_CLIENT_ERRORS_PASS cases=3 reconnect=0")


if __name__ == "__main__":
    main()
