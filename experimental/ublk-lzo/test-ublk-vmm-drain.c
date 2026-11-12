/* CPU-only direct-step integration test for VMM-backed target draining. */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>

#define cuda_vmm_pool_release_block fixture_release_block
#define cuda_vmm_pool_destroy fixture_destroy_pool
#define cuda_vmm_pool_block_allocated fixture_block_allocated
#include "cuda-vmm-pool.h"
int main(int, char **) __asm__("unused_ublk_target_main");
int vmm_drain_test_main(void) __asm__("main");
int fixture_release_block(struct cuda_vmm_pool *, size_t);
int fixture_destroy_pool(struct cuda_vmm_pool **);
static ssize_t fixture_pwrite(int, const void *, size_t, off_t);
static ssize_t fixture_pread(int, void *, size_t, off_t);
static int fixture_fsync(int);
#define pwrite fixture_pwrite
#define pread fixture_pread
#define fsync fixture_fsync
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ublk-lzo-target.c"
#pragma GCC diagnostic pop
#undef main
#undef cuda_vmm_pool_release_block
#undef cuda_vmm_pool_destroy
#undef cuda_vmm_pool_block_allocated
#undef pwrite
#undef pread
#undef fsync

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _Exit(90); } } while (0)
enum { BACK_FD = 123456, PAGES = 2048, LOGICAL_BYTES = PAGES * COMP_PAGE,
       PHYSICAL_BYTES = 4 * 1024 * 1024, BLOCK_BYTES = 2 * 1024 * 1024 };
enum { CASE_SUCCESS, CASE_FSYNC_ERROR, CASE_RELEASE_ERROR,
       CASE_DISK_ENOSPC, CASE_DISK_SHORT_WRITE };
static int scenario;
static unsigned char backing[16 * 1024 * 1024];
static unsigned char expected[PAGES][COMP_PAGE];
static unsigned char replacement[COMP_PAGE];
static unsigned char *fake_vram;
static struct cuda_vmm_pool *pool_token = (struct cuda_vmm_pool *)(uintptr_t)0x1234;
static int released[2], fsync_acked, release_calls, sync_calls, copy_calls, early_release_block = -1;
static int fake_fsync_calls, injected_error, injected_write, premature_release;
static uint64_t fail_page, fail_refs;
static struct pte fail_pte;
static unsigned long fail_raw, fail_compressed, fail_disk;

int fixture_block_allocated(const struct cuda_vmm_pool *pool, size_t block)
{
    CHECK(pool == pool_token && block < 2);
    return !released[block];
}

/* The distinct addresses are intentional: CUDA-backend validation rejects the
 * mock-backend wrappers even though these functions also copy CPU memory. */
static CUresult cpu_upload(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream; ++copy_calls;
    CHECK(dst >= (CUdeviceptr)(uintptr_t)fake_vram &&
          dst - (CUdeviceptr)(uintptr_t)fake_vram <= PHYSICAL_BYTES &&
          n <= PHYSICAL_BYTES - (dst - (CUdeviceptr)(uintptr_t)fake_vram));
    size_t off = dst - (CUdeviceptr)(uintptr_t)fake_vram;
    CHECK(!released[off / BLOCK_BYTES]);
    memcpy((void *)(uintptr_t)dst, src, n); return CUDA_SUCCESS;
}
static CUresult cpu_download(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream; ++copy_calls;
    CHECK(src >= (CUdeviceptr)(uintptr_t)fake_vram &&
          src - (CUdeviceptr)(uintptr_t)fake_vram <= PHYSICAL_BYTES &&
          n <= PHYSICAL_BYTES - (src - (CUdeviceptr)(uintptr_t)fake_vram));
    size_t off = src - (CUdeviceptr)(uintptr_t)fake_vram;
    CHECK(!released[off / BLOCK_BYTES]);
    memcpy(dst, (const void *)(uintptr_t)src, n); return CUDA_SUCCESS;
}
static CUresult cpu_sync(CUstream stream) { (void)stream; ++sync_calls; return CUDA_SUCCESS; }
static CUresult forbidden_free(CUdeviceptr p) { (void)p; CHECK(0); return 1; }

int fixture_release_block(struct cuda_vmm_pool *pool, size_t b)
{
    CHECK(pool == pool_token && b < 2 && !released[b] && fsync_acked);
    CHECK(workq.drain_state == DRAIN_RUNNING && workq.vmm_refs_ready && !workq.vmm_refs[b]);
    for (uint64_t pg = 0; pg < g_npages; ++pg) {
        struct pte e = g_ptes[pg];
        if (e.kind == PTE_RAW || e.kind == PTE_COMPRESSED)
            CHECK(e.vram_off / BLOCK_BYTES != b);
    }
    ++release_calls;
    if (scenario == CASE_DISK_ENOSPC || scenario == CASE_DISK_SHORT_WRITE)
        premature_release = 1;
    if (scenario == CASE_RELEASE_ERROR && !injected_error) {
        injected_error = 1;
        return 1;
    }
    if (early_release_block < 0) early_release_block = (int)b;
    if (b == 1) CHECK(workq.drain_cursor < PAGES);
    memset(fake_vram + b * BLOCK_BYTES, 0xa5, BLOCK_BYTES);
    released[b] = 1;
    return CUDA_SUCCESS;
}
int fixture_destroy_pool(struct cuda_vmm_pool **pool)
{
    CHECK(pool && *pool == pool_token && released[0] && released[1]);
    *pool = NULL; return CUDA_SUCCESS;
}

static void check_io(int fd, size_t n, off_t off)
{
    CHECK(fd == BACK_FD && off >= 0 && (size_t)off <= sizeof(backing) - n);
}
static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    check_io(fd, n, off);
    if (workq.drain_state == DRAIN_RUNNING && !injected_write &&
        (scenario == CASE_DISK_ENOSPC || scenario == CASE_DISK_SHORT_WRITE)) {
        injected_write = 1;
        fail_page = (uint64_t)off / (2 * COMP_PAGE);
        fail_pte = g_ptes[fail_page]; fail_refs = workq.vmm_refs[fail_pte.vram_off / BLOCK_BYTES];
        fail_raw = g_pages_raw; fail_compressed = g_pages_compressed; fail_disk = g_pages_disk;
        if (scenario == CASE_DISK_ENOSPC) { errno = ENOSPC; return -1; }
        memcpy(backing + off, buf, n / 2);
        return (ssize_t)(n / 2);
    }
    memcpy(backing + off, buf, n); return (ssize_t)n;
}
static ssize_t fixture_pread(int fd, void *buf, size_t n, off_t off)
{
    check_io(fd, n, off); memcpy(buf, backing + off, n); return (ssize_t)n;
}
static int fixture_fsync(int fd)
{
    CHECK(fd == BACK_FD);
    if (workq.drain_state == DRAIN_RUNNING) {
        ++fake_fsync_calls;
        if (scenario == CASE_FSYNC_ERROR) { errno = EIO; return -1; }
        fsync_acked = 1;
    }
    return 0;
}

static void make_pattern(unsigned char *p, unsigned seed, int compressible)
{
    for (size_t i = 0; i < COMP_PAGE; ++i) {
        if (compressible) p[i] = (unsigned char)((i / 64 + seed % 5) % 5);
        else { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; p[i] = (unsigned char)seed; }
    }
}
static void init_case(int which)
{
    scenario = which; alarm(10);
    g_compress = COMP_LZO; g_dedup = 0; g_nbd_threads = 1;
    g_export_size = LOGICAL_BYTES; g_vram_size = PHYSICAL_BYTES;
    fake_vram = calloc(1, PHYSICAL_BYTES); CHECK(fake_vram);
    g_vram_ptr = (CUdeviceptr)(uintptr_t)fake_vram;
    _cuMemcpyHtoDAsync = cpu_upload; _cuMemcpyDtoHAsync = cpu_download;
    _cuStreamSynchronize = cpu_sync; _cuMemFree = forbidden_free;
    CHECK(compress_init() == 0 && g_npages == PAGES);
    g_back_fd = BACK_FD;
    workq.backend = QUEUE_BACKEND_CUDA; workq.vmm_enabled = 1;
    workq.vmm_pool = pool_token; workq.vmm_count = 2; workq.vmm_block_bytes = BLOCK_BYTES;
    workq.vmm_refs = calloc(2, sizeof(*workq.vmm_refs));
    workq.vmm_released = calloc(2, sizeof(*workq.vmm_released));
    CHECK(workq.vmm_refs && workq.vmm_released);
    workq.stage = calloc(1, compression_stage_size()); CHECK(workq.stage);
    CHECK(posix_memalign((void **)&workq.drain_page, 4096, COMP_PAGE) == 0);
    t_cstage = workq.stage; t_lzo_work = g_lzo_work[0];
    for (unsigned order = 0; order < PAGES; ++order) {
        unsigned pg = order ? (order <= 2000 ? order - 1 : order) : 2000;
        make_pattern(expected[pg], 0x81231u + pg * 97u, pg % 4 == 0);
        struct cpend op = { .pg = pg, .plain = (const char *)expected[pg] };
        prepare_plain(pg, op.plain, 0, &op);
        CHECK(write_pending(&op, 1, NULL) == 0);
    }
    CHECK(g_pages_raw + g_pages_compressed + g_pages_disk == PAGES);
    CHECK(g_ptes[2000].kind == PTE_COMPRESSED);
    CHECK(g_ptes[2000].vram_off < BLOCK_BYTES);
    CHECK(g_pages_disk > 0);
    workq.drain_state = DRAIN_RUNNING;
}
static void run_case(int which)
{
    init_case(which);
    int done = 0, saw_early = 0, overwrote = 0;
    while (!done) {
        fsync_acked = 0;
        uint32_t error = drain_mock_pool(&workq, &done);
        if (error) {
            workq.drain_error = error; workq.drain_state = DRAIN_FAILED; break;
        }
        if (released[0] || released[1]) {
            if (!saw_early) {
                saw_early = 1;
                CHECK(workq.drain_cursor < PAGES);
                if (which == CASE_SUCCESS) {
                    unsigned char got[COMP_PAGE];
                    CHECK(comp_read(2000u * COMP_PAGE, (char *)got, COMP_PAGE, NULL) == 0);
                    CHECK(!memcmp(got, expected[2000], COMP_PAGE));
                    make_pattern(replacement, 0x9e3779b9u, 0);
                    uint64_t refs_before = workq.vmm_refs[0];
                    CHECK(refs_before > 0);
                    CHECK(comp_write(2000u * COMP_PAGE, (const char *)replacement, COMP_PAGE, NULL) == 0);
                    memcpy(expected[2000], replacement, COMP_PAGE); overwrote = 1;
                    CHECK(g_ptes[2000].kind == PTE_DISK && workq.vmm_refs[0] == refs_before - 1);
                }
            }
        }
        if (which != CASE_SUCCESS && workq.drain_state == DRAIN_FAILED) break;
    }
    if (which == CASE_SUCCESS) {
        CHECK(done && workq.drain_state == DRAIN_RUNNING && saw_early && overwrote);
        CHECK(released[0] && released[1] && release_calls == 2 && early_release_block == 1);
        CHECK(workq.drain_cursor == PAGES && !workq.vmm_refs[0] && !workq.vmm_refs[1]);
        CHECK(workq.vmm_pool == pool_token && g_vram_ptr && g_slabs && g_pool_retired);
    } else {
        CHECK(!done && workq.drain_state == DRAIN_FAILED && workq.drain_error == EIO);
        CHECK(workq.vmm_pool == pool_token && !released[0] && !released[1]);
        if (which == CASE_FSYNC_ERROR) CHECK(release_calls == 0 && fake_fsync_calls == 1);
        else if (which == CASE_RELEASE_ERROR) CHECK(release_calls == 1 && injected_error);
        else {
            CHECK(injected_write && !premature_release && release_calls == 0);
            CHECK(g_ptes[fail_page].kind == fail_pte.kind &&
                  g_ptes[fail_page].vram_off == fail_pte.vram_off &&
                  g_ptes[fail_page].klass == fail_pte.klass &&
                  g_ptes[fail_page].clen == fail_pte.clen &&
                  g_ptes[fail_page].checksum == fail_pte.checksum);
            CHECK(workq.vmm_refs[fail_pte.vram_off / BLOCK_BYTES] == fail_refs);
            CHECK(g_pages_raw == fail_raw && g_pages_compressed == fail_compressed &&
                  g_pages_disk == fail_disk);
            CHECK(workq.drain_cursor == fail_page);
        }
    }
    t_cstage = workq.stage; t_lzo_work = g_lzo_work[0];
    for (unsigned pg = 0; pg < PAGES; ++pg) {
        unsigned char got[COMP_PAGE];
        CHECK(comp_read((uint64_t)pg * COMP_PAGE, (char *)got, COMP_PAGE, NULL) == 0);
        CHECK(!memcmp(got, expected[pg], COMP_PAGE));
    }
    if (which != CASE_SUCCESS) CHECK(workq.vmm_pool == pool_token);
    else CHECK(fixture_destroy_pool(&workq.vmm_pool) == 0 && !workq.vmm_pool);
    free(fake_vram); free(g_slabs); free(g_ptes); free(g_lzo_work[0]);
    free(workq.vmm_refs); free(workq.vmm_released); free(workq.stage); free(workq.drain_page);
    CHECK(dlclose(g_liblzo) == 0);
}

int vmm_drain_test_main(void)
{
    for (int which = CASE_SUCCESS; which <= CASE_DISK_SHORT_WRITE; ++which) {
        fflush(NULL);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { run_case(which); _Exit(0); }
        int status; pid_t got;
        do got = waitpid(pid, &status, 0); while (got < 0 && errno == EINTR);
        CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("VMM drain CPU step fixture: PASS (GPU=0 threads=0)");
    return 0;
}
