/* Disposable-VM fixture only: no CUDA, GPU, device access, or real pinning.
 * Build: cc -std=c11 -Wall -Wextra -Werror -shared -fPIC
 *        test-libcuda-fixture.c -o /tmp/libcuda-fixture.so.1
 * Device payload: 2 MiB total; host staging: 64 MiB total.
 * shortcut: async copies complete immediately, use hardware for ordering tests.
 * CUDA_FIXTURE_FAIL_INIT=1 makes cuInit return CUDA_ERROR_UNKNOWN.
 * CUDA_FIXTURE_LOW_FREE=1 reports free memory below the retirement threshold.
 * CUDA_FIXTURE_LOW_FREE=after-marker waits for /tmp/cuda-low-free to exist.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
enum { OK = 0, INVALID = 1, OOM = 2, NOT_INIT = 3,
       BAD_DEVICE = 101, BAD_CONTEXT = 201, BAD_HANDLE = 400, UNKNOWN = 999 };
#define DEVICE_LIMIT ((size_t)2 * 1024 * 1024)
/* Simulated capacity for polling only; allocations retain the hard 2 MiB cap. */
#define REPORTED_TOTAL ((size_t)16 * 1024 * 1024 * 1024)
#define HOST_LIMIT ((size_t)64 * 1024 * 1024)
#define SLOTS 256
static atomic_flag mutex = ATOMIC_FLAG_INIT;
static int initialized;
static uintptr_t next_handle = 1;
static _Thread_local CUcontext current;
static CUcontext contexts[SLOTS];
static struct { CUstream id; CUcontext owner; } streams[SLOTS];
static struct { void *ptr; size_t size; CUcontext owner; int host; } blocks[SLOTS];
static size_t device_used, host_used;
static void lock(void) { while (atomic_flag_test_and_set(&mutex)) {} }
static void unlock(void) { atomic_flag_clear(&mutex); }
static int context_valid(CUcontext ctx)
{
    for (int i = 0; i < SLOTS; ++i)
        if (ctx && contexts[i] == ctx) return 1;
    return 0;
}
static int ready(void)
{
    return !initialized ? NOT_INIT : context_valid(current) ? OK : BAD_CONTEXT;
}
static int stream_valid(CUstream stream)
{
    if (!stream) return 1;
    for (int i = 0; i < SLOTS; ++i)
        if (streams[i].id == stream && streams[i].owner == current) return 1;
    return 0;
}
static int held_copy_pause(void)
{
    const char *enabled = getenv("CUDA_FIXTURE_COPY_BARRIER");
    if (!enabled || strcmp(enabled, "1")) return OK;
    if (access("/tmp/cuda-copy-arm", F_OK) != 0) return OK;
    if (access("/tmp/cuda-copy-ready", F_OK) == 0 ||
        access("/tmp/cuda-copy-release", F_OK) == 0) return INVALID;
    if (unlink("/tmp/cuda-copy-arm") != 0) return INVALID;
    int fd = open("/tmp/cuda-copy-ready", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return INVALID;
    close(fd);
    struct timespec start, now, delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return UNKNOWN;
    do {
        if (access("/tmp/cuda-copy-release", F_OK) == 0) return OK;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UNKNOWN;
        if (now.tv_sec - start.tv_sec >= 10) return UNKNOWN;
        nanosleep(&delay, NULL);
    } while (1);
}
CUresult cuInit(unsigned int flags)
{
    puts("CUDA_FIXTURE_NOT_REAL");
    fflush(stdout);
    if (flags) return INVALID;
    const char *fail = getenv("CUDA_FIXTURE_FAIL_INIT");
    if (fail && strcmp(fail, "1") == 0) return UNKNOWN;
    lock(); initialized = 1; unlock();
    return OK;
}
CUresult cuDeviceGet(CUdevice *device, int ordinal)
{
    lock();
    int r = !initialized ? NOT_INIT : !device ? INVALID : ordinal ? BAD_DEVICE : OK;
    if (!r) *device = 0;
    unlock(); return r;
}
CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice device)
{
    (void)flags;
    lock();
    int r = !initialized ? NOT_INIT : !ctx ? INVALID : device ? BAD_DEVICE : OOM;
    if (r == OOM) for (int i = 0; i < SLOTS; ++i) if (!contexts[i]) {
        current = contexts[i] = (CUcontext)next_handle++;
        *ctx = current; r = OK; break;
    }
    unlock(); return r;
}
CUresult cuCtxSetCurrent(CUcontext ctx)
{
    lock();
    int r = !initialized ? NOT_INIT : ctx && !context_valid(ctx) ? BAD_CONTEXT : OK;
    if (!r) current = ctx;
    unlock(); return r;
}
CUresult cuCtxDestroy_v2(CUcontext ctx)
{
    lock();
    int r = !initialized ? NOT_INIT : !context_valid(ctx) ? BAD_CONTEXT : OK;
    if (!r) {
        for (int i = 0; i < SLOTS; ++i) {
            if (blocks[i].ptr && blocks[i].owner == ctx) {
                size_t *used = blocks[i].host ? &host_used : &device_used;
                assert(*used >= blocks[i].size);
                *used -= blocks[i].size;
                free(blocks[i].ptr); blocks[i].ptr = NULL;
            }
            if (streams[i].owner == ctx) streams[i].id = NULL;
            if (contexts[i] == ctx) contexts[i] = NULL;
        }
        if (current == ctx) current = NULL;
    }
    unlock(); return r;
}
static int allocate(void **out, size_t size, int host)
{
    int r = ready();
    if (r) return r;
    if (!out || !size) return INVALID;
    size_t *used = host ? &host_used : &device_used;
    size_t limit = host ? HOST_LIMIT : DEVICE_LIMIT;
    if (size > limit - *used) return OOM;
    for (int i = 0; i < SLOTS; ++i) if (!blocks[i].ptr) {
        void *p = malloc(size);
        if (!p) return OOM;
        blocks[i].ptr = p; blocks[i].size = size;
        blocks[i].owner = current; blocks[i].host = host;
        *used += size; *out = p;
        assert(*used <= limit);
        return OK;
    }
    return OOM;
}
CUresult cuMemAlloc_v2(CUdeviceptr *out, size_t size)
{
    lock(); void *p = NULL;
    int r = allocate(out ? &p : NULL, size, 0);
    if (!r) *out = (CUdeviceptr)(uintptr_t)p;
    unlock(); return r;
}
CUresult cuMemAllocHost_v2(void **out, size_t size)
{
    lock(); int r = allocate(out, size, 1); unlock(); return r;
}
static int release(void *ptr, int host)
{
    int r = ready();
    if (r) return r;
    for (int i = 0; i < SLOTS; ++i)
        if (ptr && blocks[i].ptr == ptr && blocks[i].host == host &&
            blocks[i].owner == current) {
            size_t *used = host ? &host_used : &device_used;
            assert(*used >= blocks[i].size);
            *used -= blocks[i].size;
            free(ptr); blocks[i].ptr = NULL; return OK;
        }
    return INVALID;
}
CUresult cuMemFree_v2(CUdeviceptr ptr)
{
    lock(); int r = release((void *)(uintptr_t)ptr, 0); unlock(); return r;
}
CUresult cuMemFreeHost(void *ptr)
{
    lock(); int r = release(ptr, 1); unlock(); return r;
}
CUresult cuStreamCreate(CUstream *out, unsigned int flags)
{
    lock(); int r = ready();
    if (!r) r = !out || flags > 1 ? INVALID : OOM;
    if (r == OOM) for (int i = 0; i < SLOTS; ++i) if (!streams[i].id) {
        streams[i].id = (CUstream)next_handle++;
        streams[i].owner = current; *out = streams[i].id; r = OK; break;
    }
    unlock(); return r;
}
CUresult cuStreamSynchronize(CUstream stream)
{
    lock(); int r = ready();
    if (!r && !stream_valid(stream)) r = BAD_HANDLE;
    unlock(); return r;
}
CUresult cuStreamDestroy_v2(CUstream stream)
{
    lock(); int r = ready();
    if (!r) {
        r = BAD_HANDLE;
        for (int i = 0; i < SLOTS; ++i)
            if (stream && streams[i].id == stream && streams[i].owner == current) {
                streams[i].id = NULL; r = OK; break;
            }
    }
    unlock(); return r;
}
static int copy(CUdeviceptr device, void *host, size_t size, CUstream stream, int to_device)
{
    int r = ready();
    if (r) return r;
    if (!stream_valid(stream)) return BAD_HANDLE;
    if (!size) return OK;
    if (!host) return INVALID;
    uintptr_t address = (uintptr_t)device;
    for (int i = 0; i < SLOTS; ++i) {
        uintptr_t base = (uintptr_t)blocks[i].ptr;
        if (base && !blocks[i].host && blocks[i].owner == current &&
            address >= base && address - base <= blocks[i].size &&
            size <= blocks[i].size - (address - base)) {
            void *p = (char *)blocks[i].ptr + (address - base);
            if (!to_device && getenv("CUDA_FIXTURE_COPY_BARRIER")) {
                unlock();
                int pause_result = held_copy_pause();
                lock();
                if (pause_result) return pause_result;
                if ((r = ready()) != OK) return r;
                if (!stream_valid(stream)) return BAD_HANDLE;
                int still_valid = 0;
                for (int j = 0; j < SLOTS; ++j)
                    if (blocks[j].ptr == (void *)base && !blocks[j].host &&
                        blocks[j].owner == current && address >= base &&
                        address - base <= blocks[j].size &&
                        size <= blocks[j].size - (address - base)) {
                        p = (char *)blocks[j].ptr + (address - base);
                        still_valid = 1; break;
                    }
                if (!still_valid) return INVALID;
            }
            if (to_device) memcpy(p, host, size);
            else memcpy(host, p, size);
            return OK;
        }
    }
    return INVALID;
}
CUresult cuMemcpyHtoDAsync_v2(CUdeviceptr dst, const void *src, size_t size, CUstream stream)
{
    lock(); int r = copy(dst, (void *)src, size, stream, 1); unlock(); return r;
}
CUresult cuMemcpyDtoHAsync_v2(void *dst, CUdeviceptr src, size_t size, CUstream stream)
{
    lock(); int r = copy(src, dst, size, stream, 0); unlock(); return r;
}
CUresult cuMemGetInfo_v2(size_t *available, size_t *total)
{
    lock(); int r = ready();
    if (!r && (!available || !total)) r = INVALID;
    if (!r) {
        const char *low_free = getenv("CUDA_FIXTURE_LOW_FREE");
        int low = low_free && (strcmp(low_free, "1") == 0 ||
                  (strcmp(low_free, "after-marker") == 0 &&
                   access("/tmp/cuda-low-free", F_OK) == 0));
        *total = REPORTED_TOTAL;
        *available = low ?
                     REPORTED_TOTAL / 8 - 1 - device_used : REPORTED_TOTAL - device_used;
    }
    unlock(); return r;
}
CUresult cuGetErrorName(CUresult code, const char **out)
{
    if (!out) return INVALID;
    switch (code) {
    case OK: *out = "CUDA_SUCCESS"; break;
    case INVALID: *out = "CUDA_ERROR_INVALID_VALUE"; break;
    case OOM: *out = "CUDA_ERROR_OUT_OF_MEMORY"; break;
    case NOT_INIT: *out = "CUDA_ERROR_NOT_INITIALIZED"; break;
    case BAD_DEVICE: *out = "CUDA_ERROR_INVALID_DEVICE"; break;
    case BAD_CONTEXT: *out = "CUDA_ERROR_INVALID_CONTEXT"; break;
    case BAD_HANDLE: *out = "CUDA_ERROR_INVALID_HANDLE"; break;
    case UNKNOWN: *out = "CUDA_ERROR_UNKNOWN"; break;
    default: *out = "CUDA_FIXTURE_UNRECOGNIZED_ERROR"; return INVALID;
    }
    return OK;
}
CUresult cuGetErrorString(CUresult code, const char **out)
{
    return cuGetErrorName(code, out);
}
