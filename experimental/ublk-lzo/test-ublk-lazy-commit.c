/* CPU-only fixture for lazy VMM commits and packed-write rollback. */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <stdarg.h>

#define cuda_vmm_pool_commit_block fixture_commit_block
#define cuda_vmm_pool_block_allocated fixture_block_allocated
#include "cuda-vmm-pool.h"
int main(int, char **) __asm__("unused_ublk_target_main");
int lazy_commit_test_main(void) __asm__("main");
int fixture_commit_block(struct cuda_vmm_pool *, size_t);
int fixture_block_allocated(const struct cuda_vmm_pool *, size_t);
static ssize_t fixture_pwrite(int, const void *, size_t, off_t);
static ssize_t fixture_pread(int, void *, size_t, off_t);
#define pwrite fixture_pwrite
#define pread fixture_pread
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ublk-lzo-target.c"
#pragma GCC diagnostic pop
#undef main
#undef cuda_vmm_pool_commit_block
#undef cuda_vmm_pool_block_allocated
#undef pwrite
#undef pread

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _Exit(90); } } while (0)
enum { BACK_FD = 123456, PAGES = 1056, BLOCK_BYTES = 2 * 1024 * 1024,
       VRAM_BYTES = 2 * BLOCK_BYTES };
enum { CASE_SAME_BLOCK, CASE_HEADROOM, CASE_COMMIT, CASE_COMMIT_AND_WRITE, CASE_HIGH_OFFSET };
static int scenario, queries, commits, copies, fail_block = -1, disk_error;
static unsigned char backing[PAGES * 2 * COMP_PAGE];
static unsigned char *fake_vram;
static unsigned char old_page[COMP_PAGE], new_page[COMP_PAGE];
static struct cuda_vmm_pool *pool_token = (struct cuda_vmm_pool *)(uintptr_t)0x1234;
static int committed[8192];

int fixture_block_allocated(const struct cuda_vmm_pool *pool, size_t block)
{ CHECK(pool == pool_token && block < workq.vmm_count && block < 8192); return committed[block]; }
int fixture_commit_block(struct cuda_vmm_pool *pool, size_t block)
{
    CHECK(pool == pool_token && block < workq.vmm_count && block < 8192); ++commits;
    committed[block] = 1;
    return (int)block == fail_block ? 71 : 0;
}
static CUresult fake_mem_info(size_t *available, size_t *total)
{
    ++queries; *total = 32u * 1024u * 1024u;
    *available = scenario == CASE_HEADROOM && queries > 1 ? 10 : *total;
    return CUDA_SUCCESS;
}
static CUresult cpu_upload(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream; ++copies;
    CHECK(dst >= (CUdeviceptr)(uintptr_t)fake_vram && dst - (CUdeviceptr)(uintptr_t)fake_vram <= VRAM_BYTES && n <= VRAM_BYTES - (dst - (CUdeviceptr)(uintptr_t)fake_vram));
    CHECK(committed[(dst - (CUdeviceptr)(uintptr_t)fake_vram) / BLOCK_BYTES]);
    memcpy((void *)(uintptr_t)dst, src, n); return CUDA_SUCCESS;
}
static CUresult cpu_download(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream; ++copies;
    CHECK(src >= (CUdeviceptr)(uintptr_t)fake_vram && src - (CUdeviceptr)(uintptr_t)fake_vram <= VRAM_BYTES && n <= VRAM_BYTES - (src - (CUdeviceptr)(uintptr_t)fake_vram));
    CHECK(committed[(src - (CUdeviceptr)(uintptr_t)fake_vram) / BLOCK_BYTES]);
    memcpy(dst, (const void *)(uintptr_t)src, n); return CUDA_SUCCESS;
}
static CUresult cpu_sync(CUstream stream) { (void)stream; return CUDA_SUCCESS; }
static void check_backing(int fd, size_t n, off_t off)
{ CHECK(fd == BACK_FD && off >= 0 && (size_t)off <= sizeof(backing) - n); }
static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    check_backing(fd, n, off);
    if (disk_error) { errno = EIO; return -1; }
    memcpy(backing + off, buf, n); return (ssize_t)n;
}
static ssize_t fixture_pread(int fd, void *buf, size_t n, off_t off)
{ check_backing(fd, n, off); memcpy(buf, backing + off, n); return (ssize_t)n; }
static CUresult forbidden_free(CUdeviceptr ptr) { (void)ptr; CHECK(0); return 1; }

static void pattern(unsigned char *p, unsigned seed)
{ for (size_t i = 0; i < COMP_PAGE; ++i) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; p[i] = (unsigned char)seed; } }
static void fill_first_vmm_block(void)
{
    unsigned char page[COMP_PAGE];
    for (unsigned i = 0; i < BLOCK_BYTES / COMP_PAGE; ++i) {
        pattern(page, 0x9e3779b9u + i * 97u);
        CHECK(comp_write((uint64_t)i * COMP_PAGE, (const char *)page, COMP_PAGE, NULL) == 0);
    }
    CHECK(committed[0] && !committed[1]);
}
static void init_case(int which)
{
    scenario = which; alarm(10); g_compress = COMP_LZO; g_dedup = 0; g_nbd_threads = 1;
    g_export_size = PAGES * COMP_PAGE;
    g_vram_size = which == CASE_HIGH_OFFSET ? (UINT64_C(16) << 30) : VRAM_BYTES;
    fake_vram = calloc(1, VRAM_BYTES); CHECK(fake_vram);
    g_vram_ptr = (CUdeviceptr)(uintptr_t)fake_vram;
    _cuMemcpyHtoDAsync = cpu_upload; _cuMemcpyDtoHAsync = cpu_download;
    _cuStreamSynchronize = cpu_sync; _cuMemFree = forbidden_free;
    _cuMemGetInfo = fake_mem_info;
    g_back_fd = BACK_FD;
    workq.backend = QUEUE_BACKEND_CUDA; workq.vmm_enabled = 1;
    workq.vmm_pool = pool_token; workq.vmm_count = g_vram_size / BLOCK_BYTES; workq.vmm_block_bytes = BLOCK_BYTES;
    committed[0] = committed[1] = 0;
    CHECK(compress_init() == 0 && g_npages == PAGES);
    t_cstage = calloc(1, compression_stage_size()); CHECK(t_cstage);
    t_lzo_work = g_lzo_work[0]; CHECK(t_lzo_work);
    g_prepare_slot = vmm_prepare_slot;
    memset(backing, 0, sizeof(backing)); queries = commits = copies = 0;
    fail_block = -1; disk_error = 0;
    pattern(old_page, 0x13579bdu); pattern(new_page, 0x2468aceu);
}
static void cleanup_case(void)
{
    free(t_cstage); t_cstage = NULL; t_lzo_work = NULL;
    free(fake_vram); fake_vram = NULL;
    free(g_ptes); g_ptes = NULL; free(g_slabs); g_slabs = NULL;
    free(g_lzo_work[0]); g_lzo_work[0] = NULL;
    for (int i = 0; i < NPLOCK; ++i) pthread_rwlock_destroy(&g_plock[i]);
    CHECK(dlclose(g_liblzo) == 0); g_liblzo = NULL;
}
static void run_case(int which)
{
    init_case(which);
    if (which == CASE_HIGH_OFFSET) {
        uint32_t last = g_nchunks - 1;
        g_slabs[last - 1].next = SLAB_NONE;
        g_slabs[last].next = g_free_chunk; g_free_chunk = last;
        uint8_t klass = NCLASS - 1;
        pthread_mutex_lock(&g_alloc_lock);
        uint64_t offset = pool_alloc(&klass);
        CHECK(offset == (UINT64_C(16) << 30) - SLAB_SZ && offset > UINT32_MAX);
        CHECK(committed[8191] && commits == 1 && queries == 1 && !copies);
        pool_free(offset, klass);
        pthread_mutex_unlock(&g_alloc_lock);
        CHECK(!g_vram_obj_bytes && g_nfree_chunks == g_nchunks && g_ptes[0].kind == PTE_NONE);
    } else if (which == CASE_SAME_BLOCK) {
        CHECK(comp_write(0, (const char *)old_page, COMP_PAGE, NULL) == 0);
        int before = queries, before_commit = commits;
        CHECK(comp_write(COMP_PAGE, (const char *)new_page, COMP_PAGE, NULL) == 0);
        CHECK(queries == before && commits == before_commit + 1 && committed[0] && !committed[1]);
    } else if (which == CASE_HEADROOM) {
        fill_first_vmm_block();
        CHECK(comp_write(BLOCK_BYTES, (const char *)new_page, COMP_PAGE, NULL) == 0);
        CHECK(queries == 2 && commits == 512 && !committed[1]);
        CHECK(g_vram_obj_bytes == BLOCK_BYTES && committed[0] && g_nfree_chunks == g_nchunks - BLOCK_BYTES / SLAB_SZ);
        CHECK(g_ptes[BLOCK_BYTES / COMP_PAGE].kind == PTE_DISK);
        unsigned char got[COMP_PAGE];
        CHECK(comp_read(BLOCK_BYTES, (char *)got, COMP_PAGE, NULL) == 0 && !memcmp(got, new_page, COMP_PAGE));
    } else {
        fill_first_vmm_block();
        int copies_before = copies;
        if (which == CASE_COMMIT) {
            fail_block = 1;
            CHECK(comp_write(BLOCK_BYTES, (const char *)new_page, COMP_PAGE, NULL) == 0);
            CHECK(copies == copies_before && g_ptes[BLOCK_BYTES / COMP_PAGE].kind == PTE_DISK);
            CHECK(g_vram_obj_bytes == BLOCK_BYTES && committed[0] && committed[1]);
            CHECK(comp_write(BLOCK_BYTES, (const char *)new_page, COMP_PAGE, NULL) == 0);
            CHECK(g_ptes[BLOCK_BYTES / COMP_PAGE].kind == PTE_DISK);
            unsigned char got[COMP_PAGE];
            CHECK(comp_read(BLOCK_BYTES, (char *)got, COMP_PAGE, NULL) == 0 && !memcmp(got, new_page, COMP_PAGE));
        } else {
            struct pte prior = g_ptes[0];
            unsigned char before[COMP_PAGE];
            CHECK(comp_read(0, (char *)before, COMP_PAGE, NULL) == 0);
            copies_before = copies;
            fail_block = 1; disk_error = 1;
            CHECK(comp_write(0, (const char *)new_page, COMP_PAGE, NULL) == EIO);
            CHECK(!memcmp(&g_ptes[0], &prior, sizeof(prior)));
            CHECK(committed[1] && copies == copies_before && g_vram_obj_bytes == BLOCK_BYTES);
            disk_error = 0;
            unsigned char got[COMP_PAGE];
            CHECK(comp_read(0, (char *)got, COMP_PAGE, NULL) == 0 && !memcmp(got, before, COMP_PAGE));
        }
    }
    cleanup_case();
}
int lazy_commit_test_main(void)
{
    for (int which = CASE_SAME_BLOCK; which <= CASE_HIGH_OFFSET; ++which) {
        fflush(NULL); pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { run_case(which); _Exit(0); }
        int status; pid_t got; do got = waitpid(pid, &status, 0); while (got < 0 && errno == EINTR);
        CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("lazy VMM commit CPU fixture: PASS (GPU=0 cgroup=0 swap=0)"); return 0;
}
