/* Real CUDA VMM lazy-commit and mlockall probe. Build only; run explicitly on hardware. */
#define _GNU_SOURCE
#include <cuda.h>
#include "cuda-vmm-pool.h"
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>

#define COPY_BYTES 4096u
#define RSS_LIMIT_BYTES (UINT64_C(128) << 20)
#define MEM_AVAILABLE_MIN_KIB (UINT64_C(4) << 20)

static int cuda_ok(CUresult rc, const char *op)
{
    const char *name = NULL, *msg = NULL;
    if (rc != CUDA_SUCCESS) { cuGetErrorName(rc, &name); cuGetErrorString(rc, &msg); }
    fprintf(stderr, "CUDA operation=%s result=%d name=%s message=%s\n", op, (int)rc,
            name ? name : (rc == CUDA_SUCCESS ? "CUDA_SUCCESS" : "unknown"), msg ? msg : "-");
    return rc == CUDA_SUCCESS;
}

#define CUDA_TRY(expr) do { if (!cuda_ok((expr), #expr)) goto cleanup; } while (0)

static int read_mem_available_kib(uint64_t *value)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256], key[64], unit[32];
    unsigned long long n;
    int found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63[^:]: %llu %31s", key, &n, unit) == 3 &&
            strcmp(key, "MemAvailable") == 0 && strcmp(unit, "kB") == 0) {
            *value = (uint64_t)n; found = 1; break;
        }
    }
    if (fclose(f) != 0) return 0;
    return found;
}

static int print_status(const char *stage, uint64_t *rss_kib)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    uint64_t lck_kib = 0;
    int rss = 0, lck = 0;
    if (!f) { perror("fopen /proc/self/status"); return 0; }
    while (fgets(line, sizeof(line), f)) {
        unsigned long long n;
        if (sscanf(line, "VmRSS: %llu kB", &n) == 1) { *rss_kib = n; rss = 1; }
        if (sscanf(line, "VmLck: %llu kB", &n) == 1) { lck_kib = n; lck = 1; }
    }
    if (fclose(f) != 0) return 0;
    if (rss && lck) printf("STATUS stage=%s VmRSS_kib=%" PRIu64 " VmLck_kib=%" PRIu64 "\n",
                           stage, *rss_kib, lck_kib);
    return rss && lck;
}

static int snapshot(const char *stage)
{
    size_t free_bytes = 0, total_bytes = 0;
    CUresult rc = cuMemGetInfo(&free_bytes, &total_bytes);
    if (!cuda_ok(rc, "cuMemGetInfo")) return 0;
    printf("METRIC stage=%s free_bytes=%zu total_bytes=%zu\n", stage, free_bytes, total_bytes);
    return 1;
}

int main(int argc, char **argv)
{
    size_t reserve_bytes = (size_t)UINT64_C(1) << 30;
    if (argc == 2 && !strcmp(argv[1], "--16g-virtual")) reserve_bytes *= 16;
    else if (argc != 1) { fprintf(stderr, "usage: %s [--16g-virtual]\n", argv[0]); return 2; }
    CUdevice dev = 0;
    CUcontext ctx = NULL;
    struct cuda_vmm_pool *pool = NULL;
    void *driver = NULL;
    uint64_t base = 0, available_kib = 0, rss_before = 0, rss_after = 0;
    struct rlimit lock_limit;
    unsigned char source[COPY_BYTES], result[COPY_BYTES];
    int retained = 0, ok = 0, cleanup_ok = 1, locked = 0;
    int memlock_changed = 0;
    struct rlimit old_memlock;
    alarm(30);

    if (geteuid() != 0) { fputs("REQUIRES_ROOT\n", stderr); goto cleanup; }
    if (!read_mem_available_kib(&available_kib) || available_kib < MEM_AVAILABLE_MIN_KIB) {
        fprintf(stderr, "INSUFFICIENT_MEMAVAILABLE kib=%" PRIu64 " required=%" PRIu64 "\n",
                available_kib, MEM_AVAILABLE_MIN_KIB); goto cleanup;
    }
    if (getrlimit(RLIMIT_MEMLOCK, &old_memlock) != 0) { perror("getrlimit RLIMIT_MEMLOCK"); goto cleanup; }
    lock_limit.rlim_cur = RLIM_INFINITY; lock_limit.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &lock_limit) != 0) { perror("setrlimit RLIMIT_MEMLOCK infinity"); goto cleanup; }
    memlock_changed = 1;

    CUDA_TRY(cuInit(0));
    CUDA_TRY(cuDeviceGet(&dev, 0));
    CUDA_TRY(cuDevicePrimaryCtxRetain(&ctx, dev)); retained = 1;
    CUDA_TRY(cuCtxSetCurrent(ctx));
    if (!snapshot("before_reserve")) goto cleanup;
    driver = dlopen("/usr/lib/libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!driver) { fprintf(stderr, "dlopen: %s\n", dlerror()); goto cleanup; }
    {
        int rc = cuda_vmm_pool_reserve(&pool, driver, (int)dev, reserve_bytes, &base);
        if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuda_vmm_pool_reserve result=%d\n", rc); goto cleanup; }
    }
    printf("VIRTUAL_RESERVE bytes=%zu\n", reserve_bytes);
    if (!snapshot("after_reserve")) goto cleanup;
    if (!print_status("STATUS before_mlockall", &rss_before)) goto cleanup;
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall MCL_CURRENT|MCL_FUTURE"); goto cleanup; }
    locked = 1;
    if (!print_status("STATUS after_mlockall", &rss_after)) goto cleanup;
    printf("METRIC rss_growth_bytes=%" PRIu64 "\n",
           rss_after > rss_before ? (rss_after - rss_before) * 1024u : 0);
    if (rss_after > rss_before && (rss_after - rss_before) * 1024u > RSS_LIMIT_BYTES) {
        fputs("ABORT excessive_RSS_growth_after_mlockall\n", stderr); goto cleanup;
    }
    if (!snapshot("after_mlockall")) goto cleanup;
    if (!pool || cuda_vmm_pool_block_count(pool) == 0) { fputs("INVALID_EMPTY_POOL\n", stderr); goto cleanup; }
    {
        size_t count = cuda_vmm_pool_block_count(pool), last = count - 1;
        if (cuda_vmm_pool_block_size(pool) > 8u * 1024u * 1024u ||
            cuda_vmm_pool_block_allocated(pool, 0) ||
            cuda_vmm_pool_commit_block(pool, last) != CUDA_SUCCESS ||
            !cuda_vmm_pool_block_allocated(pool, last)) {
            fputs("LAZY_BLOCK_STATE_FAIL\n", stderr); goto cleanup;
        }
        for (size_t i = 0; i < count; ++i) {
            if (cuda_vmm_pool_block_allocated(pool, i) != (i == last)) {
                fputs("LAZY_BLOCK_STATE_FAIL\n", stderr); goto cleanup;
            }
        }
        if (!snapshot("after_commit")) goto cleanup;
        for (size_t i = 0; i < COPY_BYTES; ++i) source[i] = (unsigned char)(i * 17u + 0x39u);
        CUDA_TRY(cuMemcpyHtoD((CUdeviceptr)(base + last * cuda_vmm_pool_block_size(pool)), source, COPY_BYTES));
        CUDA_TRY(cuCtxSynchronize());
        memset(result, 0, sizeof(result));
        CUDA_TRY(cuMemcpyDtoH(result, (CUdeviceptr)(base + last * cuda_vmm_pool_block_size(pool)), COPY_BYTES));
        if (memcmp(source, result, COPY_BYTES) != 0) { fputs("COPY_VERIFY_FAIL\n", stderr); goto cleanup; }
        printf("DATA bytes=%u verify=PASS block_allocated_0=false block_allocated_last=true\n", COPY_BYTES);
    }
    ok = 1;

cleanup:
    if (pool && retained) {
        /* Keep the primary context and driver loaded if CUDA refuses cleanup. */
        for (unsigned attempt = 0; attempt < 3 && pool; ++attempt) {
            int rc = cuda_vmm_pool_destroy(&pool);
            if (rc == CUDA_SUCCESS) break;
            fprintf(stderr, "cleanup cuda_vmm_pool_destroy attempt=%u result=%d; retaining CUDA state until exit\n",
                    attempt + 1, rc);
        }
        if (pool) cleanup_ok = 0;
    }
    if (!pool && retained) {
        if (!cuda_ok(cuCtxSetCurrent(NULL), "cleanup_cuCtxSetCurrent")) cleanup_ok = 0;
        if (!cuda_ok(cuDevicePrimaryCtxRelease(dev), "cleanup_cuDevicePrimaryCtxRelease")) cleanup_ok = 0;
    }
    if (!pool && driver && dlclose(driver) != 0) {
        fprintf(stderr, "cleanup dlclose: %s\n", dlerror()); cleanup_ok = 0;
    }
    if (locked && munlockall() != 0) { perror("cleanup munlockall"); cleanup_ok = 0; }
    if (memlock_changed && setrlimit(RLIMIT_MEMLOCK, &old_memlock) != 0) {
        perror("cleanup setrlimit RLIMIT_MEMLOCK"); cleanup_ok = 0;
    }
    if (ok && cleanup_ok) puts("PASS LAZY_CUDA_MLOCK_REAL_PASS");
    else fprintf(stderr, "RESULT test=%s cleanup=%s\n", ok ? "PASS" : "FAIL", cleanup_ok ? "PASS" : "RETAINED_UNTIL_PROCESS_EXIT");
    return ok && cleanup_ok ? 0 : 1;
}
