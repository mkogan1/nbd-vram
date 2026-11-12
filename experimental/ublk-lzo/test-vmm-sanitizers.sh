#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
: "${UBLK_SRC:?Set UBLK_SRC to the built libublksrv source tree}"
CC=${CC:-cc}
CUDA_INCLUDE=${CUDA_INCLUDE:-/opt/cuda/include}
build_dir=$(mktemp -d "${TMPDIR:-/var/tmp}/vram-sanitizers.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
       -ffunction-sections -fdata-sections -fsanitize=address,undefined)
for fixture in drain rearm; do
    mock_flags=()
    # Keep the fake CUDA symbol resolver out of ASan's own dlsym calls.
    if [[ "$fixture" == rearm ]]; then mock_flags=(-Ddlsym=fixture_dlsym); fi
    "$CC" "${flags[@]}" "${mock_flags[@]}" -I"$CUDA_INCLUDE" \
        -c cuda-vmm-pool.c -o "$build_dir/pool.o"
    "$CC" "${flags[@]}" "${mock_flags[@]}" \
        -isystem "$UBLK_SRC/include" -isystem "$UBLK_SRC" \
        "test-ublk-vmm-$fixture.c" "$build_dir/pool.o" -Wl,--gc-sections \
        -L"$UBLK_SRC/lib/.libs" -Wl,-rpath,"$UBLK_SRC/lib/.libs" \
        -lublksrv -luring -pthread -ldl -o "$build_dir/$fixture"
    # Fixtures retain process-global allocations; check invalid accesses/UB.
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/$fixture"
done
echo 'VMM_SANITIZERS_PASS address=1 undefined=1 leak_check=0 GPU=0 devices=0'
