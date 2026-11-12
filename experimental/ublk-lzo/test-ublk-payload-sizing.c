/* Mock-only actual parser/startup/cleanup test. No CUDA library is loaded.
 * cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 *   -isystem /var/tmp/ublk-compare-src/include -isystem /var/tmp/ublk-compare-src \
 *   test-ublk-payload-sizing.c -Wl,--gc-sections \
 *   -L/var/tmp/ublk-compare-src/lib/.libs -Wl,-rpath,/var/tmp/ublk-compare-src/lib/.libs \
 *   -lublksrv -luring -pthread -ldl -o /var/tmp/test-ublk-payload-sizing
 * /var/tmp/test-ublk-payload-sizing
 * Setup stops at stubbed mlockall or after synthetic backing preparation.
 * Late-setup cases use the real CPU LZO codec; filesystem and GPU calls are stubbed.
 * Worker creation is stubbed to fail; no worker or device is started.
 * Allocation stubs use sentinels, including at 16 GiB: no payload is allocated.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define UBLK_GUEST_GUARD_H
static int ublk_guest_guard(void);
static void *fixture_dlopen(const char *, int);
static void *fixture_dlsym(void *, const char *);
static int fixture_dlclose(void *);
static int fixture_mlockall(int);
static void fixture_free(void *);
static int fixture_open(const char *, int, ...);
static int fixture_close(int);
static int fixture_unlink(const char *);
static int fixture_fstatfs(int, struct statfs *);
static int fixture_fstat(int, struct stat *);
static int fixture_statx(int, const char *, int, unsigned, struct statx *);
static int fixture_fallocate(int, int, off_t, off_t);
static int fixture_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int main(int, char **) __asm__("unused_ublk_target_main");
#define dlopen fixture_dlopen
#define dlsym fixture_dlsym
#define dlclose fixture_dlclose
#define mlockall fixture_mlockall
#define free fixture_free
#define open fixture_open
#define close fixture_close
#define unlink fixture_unlink
#define fstatfs fixture_fstatfs
#define fstat fixture_fstat
#define statx(...) fixture_statx(__VA_ARGS__)
#define fallocate fixture_fallocate
#define pthread_create fixture_pthread_create
#include "ublk-lzo-target.c"
#undef free
#undef open
#undef close
#undef unlink
#undef fstatfs
#undef fstat
#undef statx
#undef fallocate
#undef pthread_create
#undef dlopen
#undef dlsym
#undef dlclose
#undef mlockall

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _Exit(90); \
} } while (0)

static unsigned guards, queries, allocs, host_allocs, frees, host_frees;
static unsigned destroys, closes, locks, binds, syncs;
static size_t requested;
static size_t reported_total = (size_t)32 * 1024 * 1024 * 1024;
static size_t reported_free[3];
static int late_setup;
static unsigned backing_opens, backing_closes, worker_attempts, drain_frees;
static off_t backing_bytes;
static int fail_query, missing_query, fail_alloc, fail_host, invalid_post_total;
static int fail_bind, fail_sync, fail_free, fail_host_free, fail_destroy;
static int fail_init, fail_context;
static char context_token, payload_token, stage_token, library_token;

static int ublk_guest_guard(void) { ++guards; return 0; }
static void *fixture_dlopen(const char *path, int flags)
{
    CHECK(flags == RTLD_NOW);
    if (late_setup && !strcmp(path, "liblzo2.so.2")) return dlopen(path, flags);
    CHECK(!strcmp(path, "libcuda.so.1"));
    return &library_token;
}
static int fixture_dlclose(void *handle)
{
    if (handle != &library_token) { CHECK(late_setup); return dlclose(handle); }
    ++closes; return 0;
}
static int fixture_mlockall(int flags)
{
    CHECK(flags == (MCL_CURRENT | MCL_FUTURE));
    ++locks; if (late_setup) return 0;
    errno = EPERM; return -1;
}
static CUresult init(unsigned flags) { CHECK(!flags); return fail_init; }
static CUresult device(CUdevice *out, int ordinal)
{ CHECK(!ordinal); *out = 0; return 0; }
static CUresult context(CUcontext *out, unsigned flags, CUdevice dev)
{
    CHECK(flags == CU_CTX_SCHED_BLOCKING_SYNC && !dev);
    if (fail_context) return 1;
    *out = (CUcontext)&context_token; return 0;
}
static CUresult bind_context(CUcontext ctx)
{ CHECK(ctx == (CUcontext)&context_token); ++binds; CHECK(!syncs && !host_frees && !frees && !destroys); return fail_bind; }
static CUresult destroy(CUcontext ctx)
{ CHECK(ctx == (CUcontext)&context_token); ++destroys; CHECK(binds == 1 && syncs == 1 && frees == (allocs && !fail_alloc) &&
        host_frees == (host_allocs && !fail_host)); return fail_destroy; }
static CUresult query(size_t *available, size_t *total)
{
    CHECK(queries < 3);
    if (queries == 2) CHECK(late_setup && backing_opens == 1 &&
        backing_bytes == 2 * (off_t)g_export_size && workq.drain_page && workq.stage);
    *available = reported_free[queries++];
    *total = invalid_post_total && queries == 2 ? 7 : reported_total;
    return fail_query == (int)queries ? 1 : 0;
}
static CUresult allocate(CUdeviceptr *out, size_t size)
{
    ++allocs; requested = size;
    if (fail_alloc) return 2;
    *out = (CUdeviceptr)(uintptr_t)&payload_token; return 0;
}
static CUresult allocate_host(void **out, size_t size)
{
    CHECK(size == compression_stage_size()); ++host_allocs;
    if (fail_host) return 2;
    *out = &stage_token; return 0;
}
static CUresult release(CUdeviceptr ptr)
{
    CHECK(ptr == (CUdeviceptr)(uintptr_t)&payload_token);
    ++frees; CHECK(binds == 1 && syncs == 1 && host_frees == (host_allocs && !fail_host) && !destroys); return fail_free;
}
static CUresult release_host(void *ptr)
{ CHECK(ptr == &stage_token); ++host_frees; CHECK(binds == 1 && syncs == 1 && !frees && !destroys); return fail_host_free; }
static CUresult sync_stream(CUstream stream)
{ CHECK(!stream); ++syncs; CHECK(binds == 1 && !host_frees && !frees && !destroys); return fail_sync; }
static CUresult forbidden(void) { CHECK(0); return 1; }
static void *fixture_dlsym(void *handle, const char *name)
{
    if (handle != &library_token) { CHECK(late_setup); return dlsym(handle, name); }
#define SYM(n, fn) if (!strcmp(name, n)) return (void *)fn
    SYM("cuInit", init); SYM("cuDeviceGet", device);
    SYM("cuCtxCreate_v2", context); SYM("cuCtxDestroy_v2", destroy);
    SYM("cuCtxSetCurrent", bind_context); SYM("cuMemAlloc_v2", allocate);
    SYM("cuMemFree_v2", release); SYM("cuMemAllocHost_v2", allocate_host);
    SYM("cuMemFreeHost", release_host); SYM("cuStreamSynchronize", sync_stream);
    if (!strcmp(name, "cuMemGetInfo_v2")) return missing_query ? NULL : (void *)query;
    if (!strcmp(name, "cuMemcpyHtoDAsync_v2") ||
        !strcmp(name, "cuMemcpyDtoHAsync_v2") ||
        !strcmp(name, "cuStreamCreate") ||
        !strcmp(name, "cuStreamDestroy_v2") || !strcmp(name, "cuGetErrorString"))
        return (void *)forbidden;
#undef SYM
    CHECK(0); return NULL;
}

static void fixture_free(void *ptr)
{
    if (ptr && ptr == workq.drain_page) ++drain_frees;
    free(ptr);
}

/* Synthetic ext4 backing: no file is opened, allocated, or written. */
static int fixture_open(const char *path, int flags, ...)
{
    CHECK(late_setup && !strcmp(path, "/never-accessed"));
    CHECK(flags == (O_TMPFILE | O_EXCL | O_RDWR | O_DIRECT | O_CLOEXEC));
    ++backing_opens; return 12345;
}
static int fixture_close(int fd)
{ CHECK(late_setup && fd == 12345); ++backing_closes; return 0; }
static int fixture_unlink(const char *path)
{ CHECK(!strcmp(path, STATUS_PATH) || !strcmp(path, STATUS_TMP)); return 0; }
static int fixture_fstatfs(int fd, struct statfs *fs)
{ CHECK(fd == 12345); memset(fs, 0, sizeof(*fs)); fs->f_type = EXT4_SUPER_MAGIC; return 0; }
static int fixture_statx(int fd, const char *path, int flags, unsigned mask, struct statx *sx)
{
    CHECK(fd == 12345 && !*path && flags == AT_EMPTY_PATH && mask == STATX_DIOALIGN);
    memset(sx, 0, sizeof(*sx)); sx->stx_mask = STATX_DIOALIGN;
    sx->stx_dio_mem_align = sx->stx_dio_offset_align = 4096; return 0;
}
static int fixture_fallocate(int fd, int mode, off_t offset, off_t bytes)
{ CHECK(fd == 12345 && !mode && !offset); backing_bytes = bytes; return 0; }
static int fixture_fstat(int fd, struct stat *st)
{
    CHECK(fd == 12345); memset(st, 0, sizeof(*st));
    st->st_size = backing_bytes; st->st_blocks = backing_bytes / 512; return 0;
}
static int fixture_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                  void *(*fn)(void *), void *arg)
{
    CHECK(late_setup && queries == 3 && thread == &workq.worker &&
          !attr && fn == worker && arg == &workq);
    ++worker_attempts; return EAGAIN;
}

static void startup(const char *value, int cuda, int duplicate, int expected)
{
    char *args[14] = { "payload-test", "--device", "0", "--size", "4194304",
                       "--backing", "/never-accessed" };
    int count = 7;
    if (cuda) args[count++] = "--cuda";
    if (value) { args[count++] = "--gpu-payload-bytes"; args[count++] = (char *)value; }
    if (duplicate) { args[count++] = "--gpu-payload-bytes"; args[count++] = "65536"; }
    args[count] = NULL;
    workq.backend = QUEUE_BACKEND_MOCK;
    CHECK(main(count, args) == expected);
}

static void parser_cases(void)
{
    const char *invalid[] = { "", "0", "65535", "65537", "17179869185", "17179934720",
        "-65536", "+65536", " 65536", "65536 ", "65536\n", "64KiB", "1e6",
        "18446744073709551615", "18446744073709551616" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        optind = 0; startup(invalid[i], 1, 0, 2);
    }
    optind = 0; startup("65536", 0, 0, 2);
    optind = 0; startup("65536", 1, 1, 2);
    char *missing[] = { "payload-test", "--gpu-payload-bytes", NULL };
    optind = 0; CHECK(main(2, missing) == 2);
    CHECK(!guards && !queries && !allocs && !closes && !locks);
}

/* Each scenario has fresh globals and cannot inherit target signal-mask changes. */
static void run_case(unsigned which)
{
    const char *value = NULL;
    unsigned eq = 2, ea = 1, eh = 1, ef = 1, ed = 1, el = 0;
    size_t payload = GPU_PAYLOAD_DEFAULT;
    reported_free[0] = vram_reserve(reported_total) + payload;
    reported_free[1] = vram_reserve(reported_total);
    switch (which) {
    case 0: parser_cases(); return;
    case 1: el = 1; break; /* default and exact reserve equality */
    case 2: value = "65536"; payload = 65536; el = 1; break;
    case 3: value = "17179869184"; payload = GPU_PAYLOAD_MAX; el = 1; break;
    case 4: --reported_free[0]; eq = 1; ea = eh = ef = 0; break;
    case 5: reported_free[0] = vram_reserve(reported_total) - 1;
            eq = 1; ea = eh = ef = 0; break;
    case 6: missing_query = 1; eq = ea = eh = ef = 0; break;
    case 7: fail_query = 1; eq = 1; ea = eh = ef = 0; break;
    case 8: reported_total = 7; eq = 1; ea = eh = ef = 0; break;
    case 9: reported_free[0] = reported_total + 1;
            eq = 1; ea = eh = ef = 0; break;
    case 10: fail_alloc = 1; eq = 1; eh = ef = 0; break;
    case 11: fail_host = 1; eq = 1; break;
    case 12: --reported_free[1]; break;
    case 13: fail_query = 2; break;
    case 14: reported_free[1] = reported_total + 1; break;
    case 15: fail_init = 1; eq = ea = eh = ef = ed = 0; break;
    case 16: fail_context = 1; eq = ea = eh = ef = ed = 0; break;
    case 17: fail_bind = 1; break;
    case 18: fail_sync = 1; break;
    case 19: fail_host_free = 1; break;
    case 20: fail_free = 1; break;
    case 21: fail_destroy = 1; break;
    case 22: value = "131072"; payload = 131072; el = 1; break;
    case 23: value = "17179803648"; payload = 17179803648; el = 1; break;
    case 24: value = "00065536"; payload = 65536; el = 1; break;
    case 25: reported_total = 0; eq = 1; ea = eh = ef = 0; break;
    case 26: invalid_post_total = 1; break;
    case 27: case 28: case 29:
        late_setup = 1; el = 1; eq = 3;
        reported_free[2] = vram_reserve(reported_total) - (which == 27);
        if (which == 29) fail_query = 3;
        break;
    case 30: value = "1073741824"; payload = 1073741824; el = 1; break;
    case 31: value = "17179869184"; payload = GPU_PAYLOAD_MAX;
        reported_total = reported_free[0] = (size_t)16 * 1024 * 1024 * 1024;
        eq = 1; ea = eh = ef = 0; break;
    case 32: value = "2147483648"; payload = 2147483648; el = 1; break;
    default: CHECK(0);
    }
    if (which == 2 || which == 3 || which == 30 || which == 32 || (which >= 22 && which <= 24)) reported_free[0] = vram_reserve(reported_total) + payload;
    /* Cleanup failures deliberately _Exit(1); verify exact short-circuit order. */
    if (which >= 17 && which <= 21) {
        /* Stubs verify cleanup order; parent verifies fail-closed exit. */
        startup(value, 1, 0, 1);
        CHECK(0);
    }
    startup(value, 1, 0, 1);
    CHECK(guards == 1 && queries == eq && allocs == ea && host_allocs == eh);
    CHECK(frees == ef && host_frees == (eh && !fail_host) && destroys == ed);
    CHECK(binds == ed && syncs == ed && closes == 1 && locks == el);
    CHECK(!ea || requested == payload);
    if (late_setup) {
        CHECK(backing_opens == 1 && backing_closes == 1 && g_back_fd == -1 && drain_frees == 1);
        CHECK(!g_ptes && !g_slabs && !g_lzo_work[0]);
        CHECK(worker_attempts == (which == 28) && !workq.started && !queue_status);
    }
}

int payload_sizing_test_main(void) __asm__("main");
int payload_sizing_test_main(void)
{
    CHECK(vram_reserve(0) == 0 && vram_reserve(7) == 0 && vram_reserve(8) == 1);
    CHECK(vram_reserve(8ULL << 30) == (1ULL << 30));
    CHECK(vram_reserve(16ULL << 30) == (2ULL << 30));
    CHECK(vram_reserve(24ULL << 30) == (2ULL << 30));
    _cuMemGetInfo = query;
    reported_free[0] = reported_free[1] = vram_reserve(reported_total);
    poll_free_vram(); CHECK(workq.drain_state == DRAIN_IDLE);
    reported_free[1]--; poll_free_vram();
    CHECK(workq.drain_state == DRAIN_PENDING);
    workq.drain_state = DRAIN_IDLE; queries = 0; fail_query = 1;
    poll_free_vram(); CHECK(workq.drain_state == DRAIN_PENDING);
    workq.drain_state = DRAIN_IDLE; queries = 0; fail_query = 0;
    _cuMemGetInfo = NULL;
    poll_free_vram(); CHECK(workq.drain_state == DRAIN_PENDING);
    workq.drain_state = DRAIN_IDLE; workq.next_seq = 0;
    for (unsigned i = 0; i <= 32; ++i) {
        fflush(NULL);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { queries = 0; alarm(5); run_case(i); _Exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (i >= 17 && i <= 21 ? 1 : 0));
        printf("PASS payload-case-%u\n", i);
    }
    puts("MOCK_ONLY_PASS parser/startup/cleanup; no GPU/devices/backing/swap");
    return 0;
}
