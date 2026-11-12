#!/usr/bin/env python3
import os
import pathlib
import shutil
import subprocess
import tempfile


SOURCE = pathlib.Path(__file__).with_name("vram-swap-ublk-sleep.sh")
PROGRESS = "/run/vram-swap-ublk-sleep-in-progress"
RESTORE = "/run/vram-swap-ublk-restore"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(name, state="inactive", pid="0", result="success", command=None,
             expected=0, initial_restore=False):
    with tempfile.TemporaryDirectory(prefix="ublk-sleep-") as temp:
        root = pathlib.Path(temp)
        run = root / "run"
        run.mkdir()
        bindir = root / "bin"
        bindir.mkdir()
        script = root / "hook.sh"
        sysblock = root / "sysblock"
        sysblock.mkdir()
        if name == "orphan-device":
            (sysblock / "ublkb0").touch()
        script.write_text(SOURCE.read_text().replace("/run/", f"{run}/").replace("/sys/class/block/", f"{sysblock}/"))
        mock = bindir / "systemctl"
        mock.write_text("""#!/bin/sh
set -eu
printf '%s\\n' "$*" >> "$MOCK_LOG"
case "$1" in
  show)
    case "$3" in
      --property=ActiveState) cat "$MOCK_STATE" ;;
      --property=Result) cat "$MOCK_RESULT" ;;
      --property=MainPID) cat "$MOCK_PID" ;;
      *) exit 90 ;;
    esac ;;
  stop)
    [ "$(cat "$MOCK_STOP_FAIL")" = 0 ] || exit 1
    printf '%s' inactive > "$MOCK_STATE"
    printf '%s' success > "$MOCK_RESULT"
    printf '%s' 0 > "$MOCK_PID" ;;
  start)
    [ "$(cat "$MOCK_START_FAIL")" = 0 ] || exit 1
    printf '%s' active > "$MOCK_STATE"
    printf '%s' 1 > "$MOCK_PID" ;;
  *) exit 91 ;;
esac
""")
        mock.chmod(0o755)
        env = os.environ.copy()
        env.update({"PATH": f"{bindir}:{os.environ['PATH']}",
                    "MOCK_STATE": str(root / "mock_state"), "MOCK_PID": str(root / "mock_pid"),
                    "MOCK_RESULT": str(root / "mock_result"), "MOCK_STOP_FAIL": str(root / "mock_stop_fail"),
                    "MOCK_START_FAIL": str(root / "mock_start_fail"), "MOCK_LOG": str(root / "log")})
        if initial_restore:
            (run / pathlib.Path(RESTORE).name).touch()
        for key, value in {"mock_state": state, "mock_pid": pid, "mock_result": result,
                           "mock_stop_fail": "1" if command == "stop-failure" else "0",
                           "mock_start_fail": "1" if command == "start-failure" else "0"}.items():
            (root / key).write_text(value)
        arg = {"active-pre": "pre", "active-post": "post", "inactive-pre": "pre",
               "stop-failure": "pre", "pid-nonzero": "pre", "start-failure": "post",
               "unknown-state": "pre", "orphan-device": "pre", "invalid-arg": "bad"}[name]
        proc = subprocess.run(["sh", str(script), arg], env=env, text=True, capture_output=True)
        check(proc.returncode == expected, f"{name}: rc {proc.returncode}, expected {expected}: {proc.stderr}")
        progress = run / pathlib.Path(PROGRESS).name
        restore = run / pathlib.Path(RESTORE).name
        if name == "inactive-pre":
            check(progress.exists() and not restore.exists(), "inactive pre markers wrong")
            check("stop " not in pathlib.Path(root / "log").read_text(), "inactive service stopped")
        elif name == "active-pre":
            check(progress.exists() and restore.exists(), "active pre must retain both markers")
            check("stop vram-swap-ublk.service" in pathlib.Path(root / "log").read_text(), "service not stopped")
        elif name == "active-post":
            check(not progress.exists() and not restore.exists(), "successful post must clear markers")
            check("start vram-swap-ublk.service" in pathlib.Path(root / "log").read_text(), "service not started")
        elif name == "stop-failure":
            check(progress.exists() and restore.exists(), f"{name}: safety markers not retained")
        elif name == "pid-nonzero":
            check(progress.exists() and not restore.exists(), "inactive PID failure must not request restore")
        elif name == "start-failure":
            check(not progress.exists() and restore.exists(), "start failure must retain restore only")
        elif name == "unknown-state":
            check(progress.exists() and not restore.exists(), "unknown state markers wrong")
        elif name == "invalid-arg":
            check(not progress.exists() and not restore.exists(), "invalid arg changed markers")
        elif name == "orphan-device":
            check(progress.exists() and not restore.exists(), "orphan device must preserve sleep inhibition")


run_case("inactive-pre")
run_case("active-pre", state="active")
run_case("active-post", initial_restore=True)
run_case("stop-failure", state="active", command="stop-failure", expected=1)
run_case("pid-nonzero", state="inactive", pid="42", expected=1)
run_case("start-failure", initial_restore=True, command="start-failure", expected=1)
run_case("unknown-state", state="maintenance", expected=1)
run_case("invalid-arg", expected=1)
run_case("orphan-device", expected=1)
print("ublk sleep hook cases passed")
