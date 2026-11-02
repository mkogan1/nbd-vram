/* CPU/RAM-only regression for incremental target draining and its sequence barrier.
 * Compile like test-ublk-drain-order.c, replacing the source and output names.
 * Uses the real LZO library and real target worker/submit; no device, GPU,
 * backing file, swap, mount, libublksrv, or queue eventfd is opened.
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

enum { BACK_FD = 123456, PAGES = 256, FIRST_LATER = 4 };
enum scenario { SUCCESS, MIGRATION_WRITE_ERROR, FINAL_FSYNC_ERROR };
static enum scenario scenario;
static unsigned char backing[2 * PAGES * COMP_PAGE];
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int first_entered, release_first, next_batch_entered, release_next, next_batch_checked;
static unsigned writes, syncs, pool_frees, slab_frees, notifications;
static void *pool, *slabs;
static char initial[PAGES][COMP_PAGE], later_existing[COMP_PAGE], later_pending[COMP_PAGE];

int ublksrv_queue_send_event(const struct ublksrv_queue *q)
{ (void)q; ++notifications; CHECK(0); return -1; }

static int fixture_prctl(int option, ...)
{ CHECK(option == PR_SET_IO_FLUSHER); return 0; }

static void fixture_free(void *p)
{
    if (p == pool) ++pool_frees;
    if (p == slabs) ++slab_frees;
    free(p);
}

static void check_io(int fd, size_t n, off_t off)
{
    CHECK(fd == BACK_FD && n == COMP_PAGE && off >= 0);
    CHECK((size_t)off <= sizeof(backing) - n && off % COMP_PAGE == 0);
}

static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    check_io(fd, n, off);
    unsigned pg = (unsigned)(off / (2 * COMP_PAGE));
    if (!first_entered) {
        CHECK(workq.drain_state == DRAIN_RUNNING && pg == 0 && g_pool_retired);
        CHECK(!pool_frees && !slab_frees);
        pthread_mutex_lock(&gate_lock);
        first_entered = 1; pthread_cond_broadcast(&gate_cond);
        while (!release_first) pthread_cond_wait(&gate_cond, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
    } else if (!next_batch_checked && pg >= 64 && workq.drain_cursor == pg) {
        pthread_mutex_lock(&workq.lock);
        CHECK(workq.jobs[0].state == JOB_DONE && workq.jobs[1].state == JOB_DONE &&
              workq.jobs[2].state == JOB_DONE);
        CHECK(workq.drain_state == DRAIN_RUNNING && workq.drain_cursor >= 64);
        CHECK(workq.drain_seq == 0 && workq.next_seq == 4);
        CHECK(workq.jobs[0].result == COMP_PAGE && workq.jobs[1].result == COMP_PAGE &&
              workq.jobs[2].result == COMP_PAGE);
        CHECK(!memcmp(workq.jobs[2].data, later_pending, COMP_PAGE));
        CHECK(!pool_frees && !slab_frees);
        pthread_mutex_unlock(&workq.lock);
        pthread_mutex_lock(&gate_lock);
        next_batch_entered = 1; pthread_cond_broadcast(&gate_cond);
        while (!release_next) pthread_cond_wait(&gate_cond, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
        CHECK(g_ptes[FIRST_LATER].kind == PTE_DISK);
        CHECK(!memcmp(backing + ((size_t)200 * 2) * COMP_PAGE,
                      later_pending, COMP_PAGE));
        next_batch_checked = 1;
    }
    if (pg == 70 && workq.drain_cursor == pg &&
        (first_entered && next_batch_checked)) {
        if (scenario == MIGRATION_WRITE_ERROR) { errno = EIO; return -1; }
    }
    memcpy(backing + off, buf, n); ++writes; return (ssize_t)n;
}

static ssize_t fixture_pread(int fd, void *buf, size_t n, off_t off)
{ check_io(fd, n, off); memcpy(buf, backing + off, n); return (ssize_t)n; }

static int fixture_fsync(int fd)
{
    CHECK(fd == BACK_FD && workq.drain_state == DRAIN_RUNNING);
    CHECK(writes >= PAGES && !pool_frees && !slab_frees && g_pool_retired);
    ++syncs;
    if (scenario == FINAL_FSYNC_ERROR) { errno = EIO; return -1; }
    return 0;
}

static void pattern(char *p, unsigned seed, int compressible)
{
    for (unsigned i = 0; i < COMP_PAGE; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        p[i] = compressible ? (char)((i / 64) % 7) : (char)seed;
    }
}

static void run(enum scenario which)
{
    scenario = which;
    alarm(5);
    g_compress = COMP_LZO; g_dedup = 0; g_nbd_threads = 1;
    g_export_size = (uint64_t)PAGES * COMP_PAGE; g_vram_size = 16 * SLAB_SZ;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size); CHECK(g_vram_ptr);
    _cuMemcpyHtoDAsync = mock_upload; _cuMemcpyDtoHAsync = mock_download;
    _cuStreamSynchronize = mock_sync;
    CHECK(compress_init() == 0 && g_npages == PAGES);
    pool = (void *)(uintptr_t)g_vram_ptr; slabs = g_slabs; g_back_fd = BACK_FD;
    workq.backend = QUEUE_BACKEND_MOCK;
    workq.stage = calloc(1, compression_stage_size()); CHECK(workq.stage);
    CHECK(posix_memalign((void **)&workq.drain_page, 4096, COMP_PAGE) == 0);
    workq.owner = pthread_self();
    t_cstage = workq.stage; t_lzo_work = g_lzo_work[0];
    for (unsigned pg = 0; pg < PAGES; pg++) {
        pattern(initial[pg], 17 + pg, pg % 3 == 1);
        struct cpend op = { .pg = pg, .plain = initial[pg] };
        prepare_plain(pg, initial[pg], 0, &op);
        CHECK(write_pending(&op, 1, NULL) == 0);
    }
    pattern(later_existing, 5001, 0); pattern(later_pending, 9001, 0);
    CHECK(g_pages_raw + g_pages_compressed == PAGES);
    pthread_mutex_lock(&workq.lock);
    workq.drain_seq = workq.next_seq++; workq.drain_state = DRAIN_PENDING;
    pthread_mutex_unlock(&workq.lock);
    CHECK(submit(&workq, 1, UBLK_IO_OP_WRITE, 0, (uint64_t)FIRST_LATER * COMP_PAGE,
                 later_existing) == 0);
    CHECK(submit(&workq, 2, UBLK_IO_OP_WRITE, 0, 200 * (uint64_t)COMP_PAGE,
                 later_pending) == 0);
    CHECK(submit(&workq, 3, UBLK_IO_OP_READ, 0, 200 * (uint64_t)COMP_PAGE,
                 NULL) == 0);
    CHECK(workq.jobs[0].seq == 1 && workq.jobs[1].seq == 2 && workq.jobs[2].seq == 3);
    pthread_mutex_lock(&workq.lock);
    CHECK(workq.drain_state == DRAIN_PENDING && workq.drain_seq == 0);
    pthread_mutex_unlock(&workq.lock);
    CHECK(pthread_create(&workq.worker, NULL, worker, &workq) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!first_entered) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    pthread_mutex_lock(&workq.lock);
    CHECK(workq.drain_state == DRAIN_RUNNING && workq.drain_seq == 0 && workq.next_seq == 4);
    /* Repeated request while RUNNING cannot change the recorded request. */
    uint64_t seq = workq.drain_seq, next = workq.next_seq;
    pthread_mutex_unlock(&workq.lock);
    request_drain(NULL, 0, 0, 0);
    pthread_mutex_lock(&workq.lock);
    CHECK(workq.drain_state == DRAIN_RUNNING && workq.drain_seq == seq && workq.next_seq == next);
    pthread_mutex_unlock(&workq.lock);
    pthread_mutex_lock(&gate_lock);
    release_first = 1; pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    pthread_mutex_lock(&workq.lock);
    while (workq.jobs[0].state != JOB_DONE || workq.jobs[1].state != JOB_DONE ||
           workq.jobs[2].state != JOB_DONE)
        pthread_cond_wait(&workq.cond, &workq.lock);
    pthread_mutex_unlock(&workq.lock);
    pthread_mutex_lock(&gate_lock);
    while (!next_batch_entered) pthread_cond_wait(&gate_cond, &gate_lock);
    CHECK(workq.drain_cursor == 64);
    request_drain(NULL, 0, 0, 0);
    CHECK(workq.drain_cursor == 64 && workq.drain_seq == seq && workq.next_seq == next);
    release_next = 1; pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);

    pthread_mutex_lock(&workq.lock);
    for (;;) {
        int done = 1;
        for (unsigned i = 0; i < 3; i++) done &= workq.jobs[i].state == JOB_DONE;
        if (done && workq.drain_state != DRAIN_RUNNING &&
            workq.drain_state != DRAIN_PENDING) break;
        pthread_cond_wait(&workq.cond, &workq.lock);
    }
    int expected_state[] = { DRAIN_OK, DRAIN_FAILED, DRAIN_FAILED };
    uint64_t expected_cursor[] = { PAGES, 70, PAGES };
    CHECK((int)workq.drain_state == expected_state[which]);
    CHECK(workq.drain_error == (which == SUCCESS ? 0 : EIO));
    CHECK(workq.drain_cursor == expected_cursor[which] &&
          workq.drain_seq == 0 && workq.next_seq == 4);
    CHECK(workq.jobs[0].seq == 1 && workq.jobs[1].seq == 2 && workq.jobs[2].seq == 3);
    CHECK(!memcmp(workq.jobs[2].data, later_pending, COMP_PAGE));
    CHECK(writes == (which == MIGRATION_WRITE_ERROR ? 72 : PAGES + 1));
    CHECK(syncs == (which == MIGRATION_WRITE_ERROR ? 0U : 1U) && next_batch_checked);
    CHECK(pool_frees == (which == SUCCESS) && slab_frees == (which == SUCCESS));
    CHECK(g_pool_retired && !notifications);
    if (which == SUCCESS) CHECK(!g_vram_ptr && !g_vram_size && !g_slabs && !g_vram_obj_bytes);
    else CHECK(g_vram_ptr == (CUdeviceptr)(uintptr_t)pool && g_slabs == slabs &&
               g_vram_size == 16 * SLAB_SZ);
    CHECK(g_ptes[FIRST_LATER].kind == PTE_DISK);
    CHECK(g_ptes[200].kind == PTE_DISK);
    workq.stopping = 1; pthread_cond_broadcast(&workq.cond);
    pthread_mutex_unlock(&workq.lock);
    CHECK(pthread_join(workq.worker, NULL) == 0);
    char got[COMP_PAGE];
    t_cstage = workq.stage; t_lzo_work = g_lzo_work[0];
    for (unsigned pg = 0; pg < PAGES; pg++) {
        CHECK(comp_read((uint64_t)pg * COMP_PAGE, got, COMP_PAGE, NULL) == 0);
        if (pg == FIRST_LATER) CHECK(!memcmp(got, later_existing, COMP_PAGE));
        else if (pg == 200) CHECK(!memcmp(got, later_pending, COMP_PAGE));
        else CHECK(!memcmp(got, initial[pg], COMP_PAGE));
    }
    if (which != SUCCESS) { free(pool); free(slabs); }
    free(g_ptes); free(g_lzo_work[0]); free(workq.stage); free(workq.drain_page);
    CHECK(dlclose(g_liblzo) == 0);
    printf("PASS %s incremental-drain-batches/sequence-barrier/queued-io\n",
           which == SUCCESS ? "success" : which == MIGRATION_WRITE_ERROR ?
           "migration-write-EIO" : "final-fsync-EIO");
    fflush(stdout); _Exit(0);
}

int drain_progress_test_main(void) __asm__("main");
int drain_progress_test_main(void)
{
    const char *names[] = { "success", "migration-write-EIO", "final-fsync-EIO" };
    for (enum scenario which = SUCCESS; which <= FINAL_FSYNC_ERROR; which++) {
        fflush(NULL);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) run(which);
        int status; pid_t result;
        do result = waitpid(pid, &status, 0); while (result < 0 && errno == EINTR);
        if (result == pid && WIFSIGNALED(status))
            fprintf(stderr, "%s child signal %d\n", names[which], WTERMSIG(status));
        CHECK(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("MOCK_ONLY_PASS real_worker=1 real_submit=1 real_lzo=1 real_GPU=0 real_device=0 real_backing=0");
    return 0;
}
