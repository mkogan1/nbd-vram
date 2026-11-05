/* CPU-only backing-store regression test; the fixture uses a buffered temporary file. */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>

static __thread int fail_write;
static ssize_t test_pwrite(int fd, const void *buf, size_t count, off_t offset);
#define pwrite test_pwrite
#include "test-metadata.h"
#undef pwrite

static ssize_t test_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (fail_write == 3) {
        fail_write = 1;
        return pwrite(fd, buf, count, offset);
    }
    if (fail_write == 1) {
        fail_write = 0;
        errno = EIO;
        return -1;
    }
    if (fail_write == 2) {
        fail_write = 0;
        if (getenv("BACKING_REAL_BTRFS")) {
            ssize_t n = pwrite(fd, buf, count, offset);
            return n == (ssize_t)count ? (ssize_t)(count / 2) : n;
        }
        return pwrite(fd, buf, count / 2, offset);
    }
    return pwrite(fd, buf, count, offset);
}

static CUresult upload(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream;
    assert(dst >= g_vram_ptr && dst - g_vram_ptr <= g_vram_size &&
           n <= g_vram_size - (dst - g_vram_ptr));
    memcpy((void *)(uintptr_t)dst, src, n);
    return CUDA_SUCCESS;
}

static CUresult download(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream;
    assert(src >= g_vram_ptr && src - g_vram_ptr <= g_vram_size &&
           n <= g_vram_size - (src - g_vram_ptr));
    memcpy(dst, (void *)(uintptr_t)src, n);
    return CUDA_SUCCESS;
}

static CUresult synchronize(CUstream stream)
{
    (void)stream;
    return CUDA_SUCCESS;
}

static void worker_init(void)
{
    t_cstage = calloc(1, compression_stage_size());
    assert(t_cstage);
    metadata_heap_forbidden = 1;
}

static void pattern(char *page, uint32_t seed)
{
    for (size_t i = 0; i < COMP_PAGE; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        page[i] = (char)seed;
    }
}

static void check_page(unsigned pg, const char *expected)
{
    char out[COMP_PAGE];
    assert(comp_read((uint64_t)pg * COMP_PAGE, out, sizeof(out), NULL) == 0);
    assert(memcmp(out, expected, sizeof(out)) == 0);
}

static pthread_barrier_t start;

static void *concurrent_worker(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    char page[COMP_PAGE], out[COMP_PAGE], fragment[128];
    worker_init();
    memset(fragment, (int)('A' + id), sizeof(fragment));
    pthread_barrier_wait(&start);
    for (unsigned pass = 0; pass < 16; pass++) {
        pattern(page, 1000 + id * 100 + pass);
        uint64_t offset = (uint64_t)(16 + id) * COMP_PAGE;
        assert(comp_write(offset, page, sizeof(page), NULL) == 0);
        assert(comp_read(offset, out, sizeof(out), NULL) == 0);
        assert(memcmp(out, page, sizeof(out)) == 0);
        offset = (uint64_t)18 * COMP_PAGE + id * sizeof(fragment);
        assert(comp_write(offset, fragment, sizeof(fragment), NULL) == 0);
    }
    free(t_cstage);
    return NULL;
}

#ifndef TEST_BACKING_NO_MAIN
int main(void)
{
    alarm(45);
    char dir[] = "/var/tmp/nbd-vram-backing-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    char path[] = "backing-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && unlink(path) == 0);

    g_nbd_threads = 2;
    g_compress = COMP_LZ4;
    g_dedup = 0;
    g_vram_size = SLAB_SZ;
    g_export_size = 32 * COMP_PAGE;
    assert(ftruncate(fd, (off_t)(2 * g_export_size)) == 0);
    g_back_fd = fd;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    assert(g_vram_ptr);
    _cuMemcpyHtoDAsync = upload;
    _cuMemcpyDtoHAsync = download;
    _cuStreamSynchronize = synchronize;
    assert(compress_init() == 0);
    worker_init();

    char page[COMP_PAGE], replacement[COMP_PAGE];
    for (unsigned pg = 0; pg < 32; pg++) {
        pattern(page, pg + 1);
        assert(comp_write((uint64_t)pg * COMP_PAGE, page, sizeof(page), NULL) == 0);
        assert(g_ptes[pg].kind == (pg < 16 ? PTE_RAW : PTE_DISK));
    }
    for (unsigned pg = 0; pg < 32; pg++) {
        pattern(page, pg + 1);
        check_page(pg, page);
    }
    assert(g_nfree_chunks == 0 && g_vram_obj_bytes == g_vram_size);

    const unsigned pg = 19;
    pattern(page, pg + 1);
    for (int failure = 1; failure <= 2; failure++) {
        uint8_t active = g_ptes[pg].fill;
        pattern(replacement, 500 + (unsigned)failure);
        fail_write = failure;
        assert(comp_write((uint64_t)pg * COMP_PAGE, replacement, sizeof(replacement), NULL) == EIO);
        assert(!fail_write && g_ptes[pg].kind == PTE_DISK && g_ptes[pg].fill == active);
        check_page(pg, page);
    }
    for (unsigned pass = 0; pass < 2; pass++) {
        uint8_t active = g_ptes[pg].fill;
        pattern(page, 600 + pass);
        assert(comp_write((uint64_t)pg * COMP_PAGE, page, sizeof(page), NULL) == 0);
        assert(g_ptes[pg].kind == PTE_DISK && g_ptes[pg].fill == !active);
        check_page(pg, page);
    }

    char first[COMP_PAGE], second[COMP_PAGE];
    pattern(first, 801); pattern(second, 802);
    struct bop requests[2] = {
        { .offset = 20 * COMP_PAGE, .len = COMP_PAGE, .cmd = NBD_CMD_WRITE, .slot = first },
        { .offset = 21 * COMP_PAGE, .len = COMP_PAGE, .cmd = NBD_CMD_WRITE, .slot = second }
    };
    fail_write = 3;
    flush_packed_ops(requests, 2, NULL);
    assert(!requests[0].error && requests[1].error == EIO && !fail_write);
    check_page(20, first);
    pattern(page, 22);
    check_page(21, page);

    pthread_t threads[2];
    assert(pthread_barrier_init(&start, NULL, 2) == 0);
    for (unsigned i = 0; i < 2; i++)
        assert(pthread_create(&threads[i], NULL, concurrent_worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < 2; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(pthread_barrier_destroy(&start) == 0);
    pattern(page, 19);
    memset(page, 'A', 128);
    memset(page + 128, 'B', 128);
    check_page(18, page);
    for (unsigned i = 0; i < 2; i++) {
        pattern(page, 1000 + i * 100 + 15);
        check_page(16 + i, page);
    }

    free(t_cstage);
    metadata_heap_forbidden = 0;
    compress_shutdown();
    assert(metadata_live_allocations == 0);
    assert(g_back_fd == -1 && backing_init("/dev/shm") != 0 && g_back_fd == -1);
    free((void *)(uintptr_t)g_vram_ptr);
    assert(chdir("/") == 0 && rmdir(dir) == 0);
    puts("PASS backing: spill, replacement, failed inactive slot, concurrency");
    return 0;
}
#endif
