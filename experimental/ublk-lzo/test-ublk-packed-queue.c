/* Partial guest-only queue adapter. No ublk device is created. The real packed
 * store is included unchanged; Btrfs setup is a test fixture, not backing_init. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
static __thread int inject_short_result;
static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pwrite fixture_pwrite
#define main original_nbd_vram_main
#include "nbd-vram.c"
#undef main
#undef pwrite
#include <linux/ublk_cmd.h>

static ssize_t fixture_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    ssize_t result = pwrite(fd, buf, n, off);
    if (inject_short_result && result == (ssize_t)n) {
        inject_short_result = 0;
        return result / 2; /* inject result only after the actual full write */
    }
    return result;
}

enum { QDEPTH = 4, TEST_PAGES = 32, JOB_FREE = 0, JOB_QUEUED, JOB_WORKING, JOB_DONE };
struct job { int state, op, result, inject; unsigned tag; uint64_t off, seq; char data[COMP_PAGE]; };
struct queue {
    pthread_mutex_t lock; pthread_cond_t cond; pthread_t worker, owner;
    int stopping, started; uint64_t next_seq; char *stage; struct job jobs[QDEPTH];
};

static CUresult mock_upload(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream;
    if (dst < g_vram_ptr || dst - g_vram_ptr > g_vram_size || n > g_vram_size - (dst - g_vram_ptr)) return 1;
    memcpy((void *)(uintptr_t)dst, src, n); return CUDA_SUCCESS;
}
static CUresult mock_download(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream;
    if (src < g_vram_ptr || src - g_vram_ptr > g_vram_size || n > g_vram_size - (src - g_vram_ptr)) return 1;
    memcpy(dst, (void *)(uintptr_t)src, n); return CUDA_SUCCESS;
}
static CUresult mock_sync(CUstream stream) { (void)stream; return CUDA_SUCCESS; }

static void *worker(void *opaque)
{
    struct queue *q = opaque;
    t_cstage = q->stage; t_lzo_work = g_lzo_work[0];
    pthread_mutex_lock(&q->lock);
    q->started = 1; pthread_cond_broadcast(&q->cond);
    for (;;) {
        struct job *j = NULL;
        for (int i = 0; i < QDEPTH; i++)
            if (q->jobs[i].state == JOB_QUEUED && (!j || q->jobs[i].seq < j->seq)) j = &q->jobs[i];
        if (!j && q->stopping) break;
        if (!j) { pthread_cond_wait(&q->cond, &q->lock); continue; }
        j->state = JOB_WORKING;
        pthread_mutex_unlock(&q->lock);
        inject_short_result = j->inject;
        uint32_t err = j->op == UBLK_IO_OP_WRITE ? comp_write(j->off, j->data, COMP_PAGE, NULL) :
                                                   comp_read(j->off, j->data, COMP_PAGE, NULL);
        int unused_injection = inject_short_result;
        inject_short_result = 0;
        pthread_mutex_lock(&q->lock);
        j->result = unused_injection ? -ENODATA : err ? -(int)err : COMP_PAGE;
        j->state = JOB_DONE; pthread_cond_broadcast(&q->cond);
    }
    pthread_mutex_unlock(&q->lock); t_cstage = NULL; t_lzo_work = NULL; return NULL;
}

/* Single queue-owner contract. Successful submissions copy the whole WRITE;
 * READ output remains in the job until the owner consumes completion. */
static int submit(struct queue *q, unsigned tag, int op, unsigned flags, uint64_t off, const void *in, int inject)
{
    if (!pthread_equal(q->owner, pthread_self())) return -EPERM;
    if (flags || (op != UBLK_IO_OP_READ && op != UBLK_IO_OP_WRITE)) return -EOPNOTSUPP;
    if (off % COMP_PAGE || off >= g_export_size || (op == UBLK_IO_OP_WRITE && !in)) return -EINVAL;
    pthread_mutex_lock(&q->lock);
    if (q->stopping) { pthread_mutex_unlock(&q->lock); return -ESHUTDOWN; }
    struct job *j = NULL;
    for (int i = 0; i < QDEPTH; i++) {
        if (q->jobs[i].state != JOB_FREE && q->jobs[i].tag == tag) { pthread_mutex_unlock(&q->lock); return -EEXIST; }
        if (q->jobs[i].state == JOB_FREE && !j) j = &q->jobs[i];
    }
    if (!j) { pthread_mutex_unlock(&q->lock); return -EAGAIN; }
    if (q->next_seq == UINT64_MAX) { pthread_mutex_unlock(&q->lock); return -EOVERFLOW; }
    j->tag = tag; j->op = op; j->off = off; j->inject = inject; j->seq = q->next_seq++;
    if (op == UBLK_IO_OP_WRITE) memcpy(j->data, in, COMP_PAGE);
    j->state = JOB_QUEUED; pthread_cond_broadcast(&q->cond); pthread_mutex_unlock(&q->lock); return 0;
}

static int complete(struct queue *q, unsigned tag, void *out)
{
    if (!pthread_equal(q->owner, pthread_self())) return -EPERM;
    pthread_mutex_lock(&q->lock);
    struct job *j = NULL;
    for (int i = 0; i < QDEPTH; i++) if (q->jobs[i].state != JOB_FREE && q->jobs[i].tag == tag) j = &q->jobs[i];
    if (!j) { pthread_mutex_unlock(&q->lock); return -ENOENT; }
    while (j->state != JOB_DONE) pthread_cond_wait(&q->cond, &q->lock);
    int result = j->result;
    if (result == COMP_PAGE && j->op == UBLK_IO_OP_READ && !out) {
        pthread_mutex_unlock(&q->lock);
        return -EINVAL;
    }
    if (result == COMP_PAGE && j->op == UBLK_IO_OP_READ && out) memcpy(out, j->data, COMP_PAGE);
    j->state = JOB_FREE; pthread_mutex_unlock(&q->lock); return result;
}

static void *wrong_owner(void *opaque)
{
    struct queue *q = opaque;
    char out[COMP_PAGE];
    if (submit(q, 900, UBLK_IO_OP_READ, 0, 0, NULL, 0) != -EPERM ||
        complete(q, 900, out) != -EPERM) return (void *)(uintptr_t)1;
    return NULL;
}

static void pattern(char *p, uint32_t x)
{
    for (int i = 0; i < COMP_PAGE; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[i] = (char)x; }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char cmdline[4096]; FILE *f = fopen("/proc/cmdline", "r");
    if (!f || !fgets(cmdline, sizeof(cmdline), f) || !strstr(cmdline, "vram_compare_guest=1")) return 2;
    fclose(f);
    int fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR | O_DIRECT | O_CLOEXEC, 0600), inode_flags;
    if (fd < 0 || ioctl(fd, FS_IOC_GETFLAGS, &inode_flags)) return 3;
    int fd_flags = fcntl(fd, F_GETFL);
    if (fd_flags < 0 || !(fd_flags & O_DIRECT)) return 3;
    inode_flags |= FS_NOCOW_FL;
    if (ioctl(fd, FS_IOC_SETFLAGS, &inode_flags) || ioctl(fd, FS_IOC_GETFLAGS, &inode_flags) ||
        !(inode_flags & FS_NOCOW_FL) || fallocate(fd, 0, 0, TEST_PAGES * COMP_PAGE * 2)) return 3;
    void *zero = NULL;
    if (posix_memalign(&zero, COMP_PAGE, COMP_PAGE)) return 3;
    memset(zero, 0, COMP_PAGE);
    for (int i = 0; i < TEST_PAGES * 2; i++) if (pwrite(fd, zero, COMP_PAGE, i * COMP_PAGE) != COMP_PAGE) return 3;
    if (fsync(fd)) return 3;
    free(zero);
    printf("PARTIAL_FIXTURE nocow=1 odirect=1 prewritten=%d alignment_and_progress_unverified=1\n", TEST_PAGES * COMP_PAGE * 2);
    g_back_fd = fd; g_nbd_threads = 1; g_compress = COMP_LZO; g_dedup = 0;
    g_vram_size = SLAB_SZ; g_export_size = TEST_PAGES * COMP_PAGE;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    _cuMemcpyHtoDAsync = mock_upload; _cuMemcpyDtoHAsync = mock_download; _cuStreamSynchronize = mock_sync;
    if (!g_vram_ptr || compress_init()) return 4;
    struct queue q = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .owner = pthread_self() };
    q.stage = calloc(1, compression_stage_size());
    if (!q.stage || pthread_create(&q.worker, NULL, worker, &q)) return 4;
    pthread_mutex_lock(&q.lock); while (!q.started) pthread_cond_wait(&q.cond, &q.lock); pthread_mutex_unlock(&q.lock);
    pthread_t stranger;
    void *stranger_result;
    if (pthread_create(&stranger, NULL, wrong_owner, &q) ||
        pthread_join(stranger, &stranger_result) || stranger_result) return 4;
    char page[COMP_PAGE], out[COMP_PAGE], original[COMP_PAGE];
    for (unsigned i = 0; i < TEST_PAGES; i++) {
        if (!i) for (int n = 0; n < COMP_PAGE; n++) page[n] = n % 71;
        else pattern(page, i + 1);
        if (submit(&q, i, UBLK_IO_OP_WRITE, 0, i * COMP_PAGE, page, 0) || complete(&q, i, NULL) != COMP_PAGE) return 5;
    }
    if (!g_pages_compressed || !g_pages_disk || g_ptes[31].kind != PTE_DISK) return 5;
    for (unsigned i = 0; i < TEST_PAGES; i++) {
        if (!i) for (int n = 0; n < COMP_PAGE; n++) page[n] = n % 71;
        else pattern(page, i + 1);
        if (submit(&q, 300 + i, UBLK_IO_OP_READ, 0, i * COMP_PAGE, NULL, 0) ||
            complete(&q, 300 + i, out) != COMP_PAGE || memcmp(page, out, COMP_PAGE)) return 5;
    }
    pattern(original, 32);
    pattern(page, 999);
    if (submit(&q, 33, UBLK_IO_OP_WRITE, 0, 31 * COMP_PAGE, page, 1) || complete(&q, 33, NULL) != -EIO ||
        submit(&q, 34, UBLK_IO_OP_READ, 0, 31 * COMP_PAGE, NULL, 0) || complete(&q, 34, out) != COMP_PAGE ||
        memcmp(out, original, COMP_PAGE)) return 6;
    void *disk_page = NULL;
    if (posix_memalign(&disk_page, COMP_PAGE, COMP_PAGE)) return 6;
    off_t disk_off = (off_t)((31 * 2 + g_ptes[31].fill) * COMP_PAGE);
    if (pread(fd, disk_page, COMP_PAGE, disk_off) != COMP_PAGE) return 6;
    ((char *)disk_page)[3] ^= 1;
    if (pwrite(fd, disk_page, COMP_PAGE, disk_off) != COMP_PAGE ||
        submit(&q, 36, UBLK_IO_OP_READ, 0, 31 * COMP_PAGE, NULL, 0) || complete(&q, 36, out) != -EIO) return 6;
    ((char *)disk_page)[3] ^= 1;
    if (pwrite(fd, disk_page, COMP_PAGE, disk_off) != COMP_PAGE ||
        submit(&q, 37, UBLK_IO_OP_READ, 0, 31 * COMP_PAGE, NULL, 0) || complete(&q, 37, out) != COMP_PAGE ||
        memcmp(out, original, COMP_PAGE)) return 6;
    free(disk_page);
    if (submit(&q, 35, UBLK_IO_OP_FLUSH, 0, 0, NULL, 0) != -EOPNOTSUPP ||
        submit(&q, 35, UBLK_IO_OP_WRITE, UBLK_IO_F_FUA, 0, page, 0) != -EOPNOTSUPP ||
        submit(&q, 35, UBLK_IO_OP_READ, 0, 1, NULL, 0) != -EINVAL) return 7;
    for (unsigned i = 0; i < QDEPTH; i++) {
        pattern(page, 700 + i);
        if (submit(&q, 400 + i, UBLK_IO_OP_WRITE, 0, 30 * COMP_PAGE, page, 0)) return 8;
    }
    for (unsigned i = 0; i < QDEPTH; i++) if (complete(&q, 400 + i, NULL) != COMP_PAGE) return 8;
    pattern(page, 703);
    if (submit(&q, 450, UBLK_IO_OP_READ, 0, 30 * COMP_PAGE, NULL, 0) ||
        complete(&q, 450, NULL) != -EINVAL ||
        complete(&q, 450, out) != COMP_PAGE || memcmp(page, out, COMP_PAGE)) return 8;
    for (unsigned i = 0; i < QDEPTH; i++) if (submit(&q, 100 + i, UBLK_IO_OP_READ, 0, i * COMP_PAGE, NULL, 0)) return 8;
    if (submit(&q, 101, UBLK_IO_OP_READ, 0, 0, NULL, 0) != -EEXIST ||
        submit(&q, 200, UBLK_IO_OP_READ, 0, 0, NULL, 0) != -EAGAIN) return 8;
    pthread_mutex_lock(&q.lock); q.stopping = 1; pthread_cond_broadcast(&q.cond); pthread_mutex_unlock(&q.lock);
    if (submit(&q, 200, UBLK_IO_OP_READ, 0, 0, NULL, 0) != -ESHUTDOWN) return 8;
    for (unsigned i = 0; i < QDEPTH; i++) if (complete(&q, 100 + i, out) != COMP_PAGE) return 8;
    if (pthread_join(q.worker, NULL)) return 8;
    printf("PARTIAL_PASS compressed=%lu spilled=%lu verified_pages=%d failure_preserved=1 disk_corruption_detected=1 ordered_writes=%d pending_drain=%d\n", g_pages_compressed, g_pages_disk, TEST_PAGES, QDEPTH, QDEPTH);
    free(q.stage); compress_shutdown(); free((void *)(uintptr_t)g_vram_ptr);
    pthread_cond_destroy(&q.cond); pthread_mutex_destroy(&q.lock); return 0;
}
