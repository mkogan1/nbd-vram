#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ -z "${UBLK_SRC:-}" ]]; then
    echo 'UBLK_SRC is required; set it to the libublksrv source/build directory' >&2
    exit 1
fi
CUDA_INCLUDE=${CUDA_INCLUDE:-/opt/cuda/include}
CC=${CC:-cc}
ublk_include=(-isystem "$UBLK_SRC/include" -isystem "$UBLK_SRC")
ublk_link=(-L"$UBLK_SRC/lib/.libs" -Wl,-rpath,"$UBLK_SRC/lib/.libs")
build_dir=$(mktemp -d "${TMPDIR:-/var/tmp}/vram-cpu-checks.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"$CC" -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$CUDA_INCLUDE" \
    -c cuda-vmm-pool.c -o "$build_dir/cuda-vmm-pool.o"
"$CC" -O2 -Wall -Wextra -Werror -I"$CUDA_INCLUDE" test-cuda-vmm-pool.c -ldl -o "$build_dir/test-cuda-vmm-pool"
"$build_dir/test-cuda-vmm-pool"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${ublk_include[@]/#/-I}" \
    test-ublk-host-pageout.c "${ublk_link[@]}" -lublksrv -luring -ldl -pthread -o "$build_dir/test-ublk-host-pageout"
"$build_dir/test-ublk-host-pageout" --self-test
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "${ublk_include[@]/#/-I}" \
    test-ublk-pageout-attribution.c -Wl,--gc-sections "${ublk_link[@]}" \
    -lublksrv -luring -ldl -pthread -o "$build_dir/test-ublk-pageout-attribution"
"$build_dir/test-ublk-pageout-attribution"
for test_name in test-backing test-backing-alignment test-backing-topology test-ublk-notify test-ublk-swap-command test-ublk-host-admission test-ublk-request-flags test-ublk-queue-failure test-ublk-drain-order test-ublk-drain-progress test-ublk-vmm-drain test-ublk-lazy-commit test-ublk-vmm-rearm test-ublk-swap-table test-ublk-auto-retire test-ublk-worker-startup test-ublk-payload-sizing test-ublk-disk-reserve test-ublk-stop-retention test-ublk-safe-stop; do
    extra_libraries=(-luring)
    if [[ "$test_name" == test-ublk-payload-sizing ]]; then
        extra_libraries=("${ublk_link[@]}" -lublksrv -luring)
    fi
    if ! "$CC" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections \
        "${ublk_include[@]}" \
        "$test_name.c" "$build_dir/cuda-vmm-pool.o" -Wl,--gc-sections "${extra_libraries[@]}" -pthread -ldl -o "$build_dir/$test_name" \
        > "$build_dir/$test_name.log" 2>&1; then
        cat -- "$build_dir/$test_name.log"
        exit 1
    fi
    if ! "$build_dir/$test_name" >> "$build_dir/$test_name.log" 2>&1; then
        cat -- "$build_dir/$test_name.log"
        exit 1
    fi
    echo "PASS $test_name"
done
python3 test-cuda-bridge-run.py
python3 test-cuda-bridge-client-errors.py
python3 test-ublk-device-digest.py
python3 test-ublk-log-relay.py
python3 test-ublk-vm-storage-admission.py
sh -n vram-swap-ublk-sleep.sh
sh -n test-nvidia-ordering.sh
python3 test-ublk-sleep.py
python3 test-ublk-qmp-wakeup-test.py
python3 test-ublk-host-probe-admission.py
python3 test-host-pageout-probe.py
echo 'CPU_ONLY_PASS GPU=0 devices=0 swap=0 mounts=0'
