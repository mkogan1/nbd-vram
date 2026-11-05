# Validation scope

Validated on 2026-11-02 with Linux 7.3.1, NVIDIA driver 615.78.08,
RTX 4070 Ti SUPER, and libublksrv
`6d0d922614ec18e6f2b5d476af8d540a1d9c1de8`.

## Passing checks for this candidate

- Existing repository `make test`: allocator, codecs, deduplication,
  batching, DMA lifetime/order and storage scripts.
- Experimental `make check-cpu`: VMM allocation/retry/cleanup, lazy commits,
  drain progress, safe STOP resource retention, admission guards, pageout
  attribution, protocol fixtures, sleep hook and guest harness checks.
- Added deterministic low-free -> worker drain -> recovered-headroom rearm;
  shutdown while rearm runs outside the queue mutex; ENOSPC and short-write
  migration failures preserving PTE/data/references and preventing release.
- `make check-sanitizers`: AddressSanitizer and UndefinedBehaviorSanitizer
  on drain/rearm fixtures. Leak detection is disabled for fixture globals.
  The fake CUDA resolver is renamed so it cannot interpose on ASan itself.
- `make check-stage`: all six packaged files/modes, service paths and order,
  no dynamic libublksrv dependency or RPATH, four unsafe staging denials.
- Native-CUDA guest VM: three swap cycles with synthetic low-free reporting,
  complete page readback and safe shutdown.
- Native-CUDA guest VM: Btrfs filled to ENOSPC, emergency reserve handling,
  migration while pages are swapped, three cycles and backing verification.
- Guest systemd with native CUDA: managed swap, guest kernel suspend/wakeup,
  evacuation before simulated NVIDIA suspend action and restoration after
  simulated NVIDIA resume action.
- Guest failure path: injected pre-sleep failure prevents both guest suspend
  and NVIDIA action entry; swapped data remains readable before clean stop.

VM commands are explicit compositions of the options in
`ublk-real-integration-vm.py --help`; they require the independently supplied
matching kernel/modules, NVKVM QEMU/module and NVIDIA userspace described in
README. They are not runnable from the source tree alone.

The native-CUDA integration uses real driver allocation/copies through an
experimental NVKVM adapter. The NVIDIA sleep actions themselves are ordering
fixtures, not physical GPU suspend. Synthetic pressure is labeled as such.

## Earlier hardware qualification of unchanged production source

A disposable host ublk target accepted a 12 GiB mixed LZO dataset. Per-process
GPU sampling measured a 9488 MiB peak from a 210 MiB baseline: 9278 MiB of
additional physical occupancy. All bytes passed before/after evacuation;
physical occupancy returned to baseline, and the normal service was restored.
This is historical hardware evidence, not a new run of the PR packaging.

## Limits

These checks cover the stated scenarios, not all possible executions. No
physical host suspend/reset, deliberate GPU loss, global host OOM, power-loss
recovery, or FPS qualification was performed. Hardware-specific success is
not a portability guarantee. Full-Btrfs tests use disposable guest storage;
they do not intentionally exhaust the desktop filesystem. The installed host
service was left running throughout the new VM/CPU validation.
