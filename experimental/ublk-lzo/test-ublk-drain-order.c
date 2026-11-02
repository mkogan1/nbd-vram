/* Safe CPU/RAM-only inclusion test of unchanged target worker/submit/drain.
 * Build from this directory (real ublksrv headers; DO NOT link libublksrv):
 * cc -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections \
 *   -I/var/tmp/ublk-compare-src/include -I/var/tmp/ublk-compare-src \
 *   test-ublk-drain-order.c -Wl,--gc-sections -pthread -ldl \
 *   -o /var/tmp/test-ublk-drain-order
 * /var/tmp/test-ublk-drain-order
 * ASan/UBSan: replace -O2 with -O1 -fsanitize=address,undefined
 * --param asan-globals=0 -fno-omit-frame-pointer (GCC). Global ASan
 * instrumentation is disabled so unused target callbacks can be discarded.
 * Fork children use _Exit: this does not qualify LeakSanitizer coverage.
 * Requires runtime liblzo2. No target main, real fd/backing, CUDA, device,
 * swap, mount, VM, queue owner/event loop or real PR_SET_IO_FLUSHER is used.
 * shortcut: SIGUSR2 request/status is an isolated seam copied from main;
 * qualify signal delivery and lifecycle separately in an authorized VM.
 * fsync failure retains the pool allocation and readable backing payload,
 * not necessarily live VRAM objects: disk_replace already commits each page.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
static ssize_t fixture_pwrite(int, const void *, size_t, off_t);
static ssize_t fixture_pread(int, void *, size_t, off_t);
static int fixture_fsync(int);
static int fixture_prctl(int, ...);
static void fixture_free(void *);
int main(int, char **) __asm__("unused_ublk_target_main");
#define pwrite fixture_pwrite
#define pread fixture_pread
#define fsync fixture_fsync
#define prctl fixture_prctl
#define free fixture_free
#include "ublk-lzo-target.c"
#undef pwrite
#undef pread
#undef fsync
#undef prctl
#undef free

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _Exit(90); \
} } while (0)

enum { BACK_FD = 123456, PAGES = 4 };
enum scenario { SUCCESS, WRITE_ERROR, SYNC_ERROR };
static enum scenario scenario;
static unsigned char backing[2 * PAGES * COMP_PAGE];
static void *pool, *slabs;
static unsigned pool_frees, slab_frees, writes, syncs, notifications;
static unsigned rejected_transfers;
static void install_rejecting_gpu(void);
static char older[2][COMP_PAGE], newer[COMP_PAGE];

/* Retained worker reference, but q->ublk stays NULL: no real eventfd. */
int ublksrv_queue_send_event(const struct ublksrv_queue *q)
{
    (void)q;
    ++notifications;
    CHECK(0);
    return -1;
}

static int fixture_prctl(int option, ...)
{
    CHECK(option == PR_SET_IO_FLUSHER);
    return 0;
}

static void fixture_free(void *p)
{
    if (p && p == pool) CHECK(++pool_frees == 1);
    if (p && p == slabs) {
        CHECK(++slab_frees == 1);
        if (scenario == SUCCESS) install_rejecting_gpu();
    }
    free(p);
}

static void check_offset(int fd, size_t n, off_t off)
{
    CHECK(fd == BACK_FD && n == COMP_PAGE && off >= 0);
    CHECK((size_t)off <= sizeof(backing) - n && off % COMP_PAGE == 0);
}

static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    check_offset(fd, n, off);
    ++writes;
    if (workq.drain_state == DRAIN_RUNNING) {
        /* Scrambled job slots must still execute strictly by sequence. */
        CHECK(workq.jobs[3].state == JOB_DONE);
        CHECK(workq.jobs[2].state == JOB_DONE);
        CHECK(workq.jobs[0].state == JOB_QUEUED);
        CHECK(workq.jobs[1].state == JOB_QUEUED);
        CHECK(g_pool_retired && !pool_frees && !slab_frees);
        CHECK(off / (2 * COMP_PAGE) < 2);
        CHECK(g_ptes[0].kind == (off == 0 ? PTE_RAW : PTE_DISK));
        CHECK(g_ptes[1].kind == PTE_COMPRESSED);
        CHECK(!memcmp(buf, older[off / (2 * COMP_PAGE)], n));
        if (scenario == WRITE_ERROR) { errno = EIO; return -1; }
    } else {
        CHECK(workq.drain_state == DRAIN_OK || workq.drain_state == DRAIN_FAILED);
    }
    memcpy(backing + off, buf, n);
    return (ssize_t)n;
}

static ssize_t fixture_pread(int fd, void *buf, size_t n, off_t off)
{
    check_offset(fd, n, off);
    memcpy(buf, backing + off, n);
    return (ssize_t)n;
}

static int fixture_fsync(int fd)
{
    CHECK(fd == BACK_FD && workq.drain_state == DRAIN_RUNNING);
    CHECK(writes == 2 && !pool_frees && !slab_frees && g_pool_retired);
    CHECK(workq.jobs[0].state == JOB_QUEUED && workq.jobs[1].state == JOB_QUEUED);
    ++syncs;
    if (scenario == SYNC_ERROR) { errno = EIO; return -1; }
    return 0;
}

/* Fail closed on every GPU API used by these non-dedup worker paths. */
static CUresult reject_upload(CUdeviceptr d, const void *s, size_t n, CUstream st)
{
    (void)d; (void)s; (void)n; (void)st;
    ++rejected_transfers; CHECK(0); return 1;
}
static CUresult reject_download(void *d, CUdeviceptr s, size_t n, CUstream st)
{
    (void)d; (void)s; (void)n; (void)st;
    ++rejected_transfers; CHECK(0); return 1;
}
static CUresult reject_sync(CUstream st)
{
    (void)st; ++rejected_transfers; CHECK(0); return 1;
}

static void install_rejecting_gpu(void)
{
    _cuMemcpyHtoDAsync = reject_upload;
    _cuMemcpyDtoHAsync = reject_download;
    _cuStreamSynchronize = reject_sync;
}

/* Caller holds q->lock; this seam does not test actual main/SIGUSR2. */
static int request_status(struct queue *q)
{
    if (q->drain_state == DRAIN_IDLE && !q->stopping && q->next_seq != UINT64_MAX) {
        q->drain_seq = q->next_seq++;
        q->drain_state = DRAIN_PENDING;
        pthread_cond_broadcast(&q->cond);
        return 1;
    }
    return 0;
}

static void pattern(char *p, unsigned seed, int compressible)
{
    for (unsigned i = 0; i < COMP_PAGE; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        p[i] = compressible ? (char)((i / 64) % 5) : (char)seed;
    }
}

static void run(enum scenario which)
{
    scenario = which;
    alarm(10);
    g_compress = COMP_LZO; g_dedup = 0; g_nbd_threads = 1;
    g_export_size = PAGES * COMP_PAGE; g_vram_size = SLAB_SZ;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    CHECK(g_vram_ptr);
    _cuMemcpyHtoDAsync = mock_upload;
    _cuMemcpyDtoHAsync = mock_download;
    _cuStreamSynchronize = mock_sync;
    CHECK(compress_init() == 0);
    pool = (void *)(uintptr_t)g_vram_ptr; slabs = g_slabs;
    g_back_fd = BACK_FD;
    workq.stage = calloc(1, compression_stage_size()); CHECK(workq.stage);
    CHECK(posix_memalign((void **)&workq.drain_page, 4096, COMP_PAGE) == 0);
    workq.owner = pthread_self();
    pattern(older[0], 17, 0); pattern(older[1], 19, 1); pattern(newer, 23, 0);
    CHECK(submit(&workq, 10, UBLK_IO_OP_WRITE, 0, 0, older[0]) == 0);
    CHECK(submit(&workq, 11, UBLK_IO_OP_WRITE, 0, COMP_PAGE, older[1]) == 0);
    pthread_mutex_lock(&workq.lock);
    CHECK(request_status(&workq) == 1 && workq.drain_seq == 2);
    CHECK(!request_status(&workq) && workq.next_seq == 3);
    pthread_mutex_unlock(&workq.lock);
    CHECK(submit(&workq, 12, UBLK_IO_OP_WRITE, 0, 2 * COMP_PAGE, newer) == 0);
    CHECK(submit(&workq, 13, UBLK_IO_OP_READ, 0, 0, NULL) == 0);
    /* Start only after submission: no timing sleeps or scheduling assumptions. */
    struct job tmp = workq.jobs[0]; workq.jobs[0] = workq.jobs[2]; workq.jobs[2] = workq.jobs[1];
    workq.jobs[1] = workq.jobs[3]; workq.jobs[3] = tmp;
    CHECK(pthread_create(&workq.worker, NULL, worker, &workq) == 0);
    pthread_mutex_lock(&workq.lock);
    for (;;) {
        int done = 1;
        for (unsigned i = 0; i < QDEPTH; i++) done &= workq.jobs[i].state == JOB_DONE;
        if (done) break;
        CHECK(pthread_cond_wait(&workq.cond, &workq.lock) == 0);
    }
    CHECK(workq.started && g_pool_retired && !notifications);
    for (unsigned i = 0; i < QDEPTH; i++) CHECK(workq.jobs[i].result == COMP_PAGE);
    CHECK(!memcmp(workq.jobs[1].data, older[0], COMP_PAGE));
    CHECK(workq.drain_state == (which == SUCCESS ? DRAIN_OK : DRAIN_FAILED));
    CHECK(workq.drain_error == (which == SUCCESS ? 0 : EIO));
    CHECK(writes == (which == WRITE_ERROR ? 2U : 3U));
    CHECK(syncs == (which == WRITE_ERROR ? 0U : 1U));
    if (which == SUCCESS) {
        CHECK(pool_frees == 1 && slab_frees == 1);
        CHECK(!g_vram_ptr && !g_vram_size && !g_slabs && !g_nchunks && !g_vram_obj_bytes);
        CHECK(_cuMemcpyHtoDAsync == reject_upload &&
              _cuMemcpyDtoHAsync == reject_download && _cuStreamSynchronize == reject_sync);
    } else {
        CHECK(!pool_frees && !slab_frees && g_vram_ptr == (CUdeviceptr)(uintptr_t)pool);
        CHECK(g_slabs == slabs && g_vram_size == SLAB_SZ);
        CHECK(g_ptes[0].kind == (which == WRITE_ERROR ? PTE_RAW : PTE_DISK));
        CHECK(g_ptes[1].kind == (which == WRITE_ERROR ? PTE_COMPRESSED : PTE_DISK));
    }
    uint8_t klass = 0;
    pthread_mutex_lock(&g_alloc_lock);
    CHECK(pool_alloc(&klass) == UINT64_MAX);
    pthread_mutex_unlock(&g_alloc_lock);
    unsigned saved_writes = writes, saved_syncs = syncs;
    for (unsigned i = 0; i < 100; i++) {
        CHECK(!request_status(&workq));
        CHECK(workq.drain_seq == 2 && workq.next_seq == 5);
        CHECK(workq.drain_error == (which == SUCCESS ? 0 : EIO));
    }
    for (unsigned i = 0; i < QDEPTH; i++) workq.jobs[i].state = JOB_FREE;
    pthread_mutex_unlock(&workq.lock);
    /* Read both original payloads and overwrite via real worker after drain.
     * SUCCESS installs rejecting APIs at slab release, before even the originally
     * queued newer jobs execute. Any transfer/sync attempt fails the test. */
    CHECK(submit(&workq, 20, UBLK_IO_OP_READ, 0, 0, NULL) == 0);
    CHECK(submit(&workq, 21, UBLK_IO_OP_READ, 0, COMP_PAGE, NULL) == 0);
    CHECK(submit(&workq, 22, UBLK_IO_OP_WRITE, 0, 2 * COMP_PAGE, older[1]) == 0);
    CHECK(submit(&workq, 23, UBLK_IO_OP_READ, 0, 2 * COMP_PAGE, NULL) == 0);
    pthread_mutex_lock(&workq.lock);
    for (unsigned i = 0; i < QDEPTH; i++)
        while (workq.jobs[i].state != JOB_DONE) pthread_cond_wait(&workq.cond, &workq.lock);
    for (unsigned i = 0; i < QDEPTH; i++) CHECK(workq.jobs[i].result == COMP_PAGE);
    CHECK(!memcmp(workq.jobs[0].data, older[0], COMP_PAGE));
    CHECK(!memcmp(workq.jobs[1].data, older[1], COMP_PAGE));
    CHECK(!memcmp(workq.jobs[3].data, older[1], COMP_PAGE));
    CHECK(writes == saved_writes + 1 && syncs == saved_syncs && !rejected_transfers);
    CHECK(pool_frees == (which == SUCCESS) && slab_frees == (which == SUCCESS));
    CHECK(g_pool_retired && !request_status(&workq));
    workq.stopping = 1; pthread_cond_broadcast(&workq.cond);
    pthread_mutex_unlock(&workq.lock);
    CHECK(pthread_join(workq.worker, NULL) == 0);
    /* Manual heap-only cleanup: compress_shutdown unlinks host status paths. */
    if (which != SUCCESS) { fixture_free(pool); fixture_free(slabs); }
    free(g_ptes); free(g_lzo_work[0]); free(workq.stage); free(workq.drain_page);
    CHECK(dlclose(g_liblzo) == 0);
    puts("PASS sequence/status/payload/closed-allocation-gate");
    fflush(stdout);
    _Exit(0);
}

int drain_order_test_main(void) __asm__("main");
int drain_order_test_main(void)
{
    const char *names[] = { "success-and-reject-post-drain-GPU", "backing-write-EIO", "fsync-EIO" };
    for (enum scenario s = SUCCESS; s <= SYNC_ERROR; s++) {
        fflush(NULL);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) run(s);
        int status; pid_t result;
        do result = waitpid(pid, &status, 0); while (result < 0 && errno == EINTR);
        CHECK(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        printf("PASS %s\n", names[s]);
    }
    puts("MOCK_ONLY_PASS real_worker=1 real_submit=1 real_drain=1 main_signal_status=0 real_GPU=0 real_backing=0");
    return 0;
}
