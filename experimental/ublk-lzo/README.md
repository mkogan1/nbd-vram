# Experimental LZO ublk target

This opt-in target stores swap pages in demand-allocated CUDA VMM blocks and
falls back to a preallocated Btrfs file. It does not change the normal NBD
build or installer. It remains experimental: a GPU reset can lose pages,
evacuation is progressive rather than instantaneous, and no FPS guarantee
is made. Do not enable it on an unattended system without qualifying its
kernel, GPU driver and backing filesystem.

## Build and CPU checks

Required: Linux, a C/C++ toolchain, Python 3, CUDA headers, liburing development
files, BusyBox (on PATH or set BUSYBOX), liblzo2 at runtime, and built
libublksrv (including its static archive).
The tested libublksrv revision is
`6d0d922614ec18e6f2b5d476af8d540a1d9c1de8` from
https://github.com/ublk-org/ublksrv. Build that revision using its own build
instructions; point UBLK_SRC at the resulting source/build tree.

```sh
export UBLK_SRC=/absolute/path/to/ublksrv
export CUDA_INCLUDE=/opt/cuda/include
make
make check-cpu
make check-sanitizers
make check-stage
make stage DESTDIR="$PWD/stage"
```

CPU checks use mocked GPU operations and disposable files; they do not create
devices, activate swap or allocate GPU memory. Staging does not install or
enable anything. The example config is deliberately conservative and is not
an automatically installed host configuration.

## Runtime model

The host target requires root, CUDA, a clean non-fixture environment,
`--kernel-safe-stop`, sufficient available RAM and a supported ublk kernel
with opener-aware TRY_STOP. Unsupported kernels fail closed. Btrfs backing
must pass the filesystem/topology admission checks. Startup preallocates and
prewrites twice the logical capacity (two slots per page), plus an emergency
reserve; this can take minutes. Metadata and the CUDA context also consume
host/GPU memory.

VMM commits physical blocks only when needed. Allocation preserves a GPU
headroom threshold (one eighth of total memory, capped at 2 GiB). Low free
memory requests migration to disk and progressively releases empty blocks.
Once sufficient headroom returns, new writes can use VRAM again. Evacuated
pages remain on disk. Exhausting the emergency disk reserve disables rearm
until restart. LZO pages have integrity checks; backing remains volatile
swap storage, not durable application storage.

The managed swap priority is 90. Stop the owning service to evacuate swap;
never SIGKILL the target or unload ublk while pages remain attached. Replace
the old NBD sleep hook before enabling the new hook. The hook refuses sleep
if evacuation or device retirement cannot be confirmed. Physical NVIDIA
suspend/reset safety and game performance require separate qualification.

## Integration tests

`python3 ublk-real-integration-vm.py --help` documents explicit guest-only
tests for Btrfs exhaustion, page integrity, repeated swap cycles, safe STOP,
systemd readiness and suspend ordering. These require QEMU/KVM, a matching
guest kernel and module tree, BusyBox, cpio, Btrfs/util-linux tools and guest
libraries. The harness builds an initramfs and disposable disk images; it
never suspends the host. Supply matching `--kernel`, `--kernel-release`,
`--target`, `--library-dir` and all selected mode inputs explicitly.

Real CUDA inside a guest additionally requires an independently built NVKVM
QEMU/module pair and matching NVIDIA userspace; those experimental external
artifacts are not distributed or automatically installed here. Fixture CUDA
tests must be labeled as simulations. Hardware receipts from one machine
do not certify other kernels or drivers.

`test-ublk-host-probe.py` is a separate explicit-root hardware diagnostic,
not part of CPU checks. It refuses existing ublk devices and creates a
disposable target. Never run raw-write tests against an active swap device.

The maintained entry points are the Makefile targets above and
`ublk-real-integration-vm.py`. Guest test sources are included; no generated
binaries, disk images, NVIDIA libraries or private host configuration ship.
