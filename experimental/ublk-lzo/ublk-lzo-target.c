#define _GNU_SOURCE
#include "ublksrv.h"
#include "ublksrv_utils.h"
#include "ublk-guest-guard.h"
#include "ublk-notify.h"
#include "ublk-swap-command.h"
#include "ublk-host-admission.h"
#include "cuda-vmm-pool.h"
/* Queue adapted from test-ublk-packed-queue.c; requests complete on the queue owner. */
#define main original_nbd_vram_main
#include "nbd-vram.c"
#undef main
#include <linux/btrfs.h>
#include <sys/statvfs.h>

enum { QDEPTH = 4, TEST_PAGES = 32, JOB_FREE = 0, JOB_QUEUED, JOB_WORKING, JOB_DONE };
struct job { int state, op, result; unsigned tag; uint64_t off, seq; char data[COMP_PAGE]; };
struct queue {
    pthread_mutex_t lock; pthread_cond_t cond; pthread_t worker, owner;
    int spare_fd, reserve_state, reserve_query_error_logged;
    enum { QUEUE_BACKEND_MOCK = 0, QUEUE_BACKEND_CUDA } backend;
    int stopping, started, queue_exited, shutdown_requested, rearm_pending; uint64_t next_seq, drain_seq;
    uint64_t drain_cursor;
    unsigned drain_jobs_left;
    int vmm_enabled, vmm_refs_ready, vmm_ref_error;
    struct cuda_vmm_pool *vmm_pool;
    size_t vmm_block_bytes, vmm_count;
    uint64_t *vmm_refs, vmm_released_bytes;
    unsigned char *vmm_released;
    enum { DRAIN_IDLE, DRAIN_PENDING, DRAIN_RUNNING, DRAIN_OK, DRAIN_FAILED, DRAIN_REARMING } drain_state;
    uint32_t drain_error; char *drain_page; char *stage; const struct ublksrv_queue *ublk; struct job jobs[QDEPTH];
};
static int trace_io;
static int test_low_free;
static struct queue *vmm_draining_queue;

static void vmm_page_freed(uint64_t offset)
{
    struct queue *q = vmm_draining_queue;
    if (!q || !q->vmm_refs_ready) return;
    if (!q->vmm_block_bytes || offset >= g_vram_size) { q->vmm_ref_error = 1; return; }
    size_t block = offset / q->vmm_block_bytes;
    if (block >= q->vmm_count || !q->vmm_refs[block] || q->vmm_released[block]) {
        q->vmm_ref_error = 1;
        return;
    }
    --q->vmm_refs[block];
}

/* The target worker is the sole PTE writer; its sequence barrier is already closed. */
static uint32_t vmm_prepare_refs(struct queue *q)
{
    if (!q->vmm_pool || q->vmm_refs_ready) return 0;
    if (!q->vmm_refs || !q->vmm_released || !q->vmm_block_bytes) return EIO;
    uint64_t compressed = 0, raw = 0;
    for (uint64_t pg = 0; pg < g_npages; pg++) {
        struct pte entry = g_ptes[pg];
        if (entry.kind != PTE_RAW && entry.kind != PTE_COMPRESSED) continue;
        if (!valid_lzo_vram_entry(entry)) return EIO;
        if (entry.kind == PTE_COMPRESSED) ++compressed; else ++raw;
        size_t block = entry.vram_off / q->vmm_block_bytes;
        if (block >= q->vmm_count) return EIO;
        ++q->vmm_refs[block];
    }
    q->vmm_refs_ready = 1;
    vmm_draining_queue = q;
    g_retired_slot_freed = vmm_page_freed;
    printf("UBLK_VMM_REFERENCES compressed=%llu raw=%llu\n",
           (unsigned long long)compressed, (unsigned long long)raw);
    return 0;
}

static uint32_t vmm_release_empty(struct queue *q)
{
    if (!q->vmm_pool) return 0;
    if (!q->vmm_refs_ready || q->vmm_ref_error) return EIO;
    pthread_mutex_lock(&g_alloc_lock);
    int candidate = 0;
    uint32_t error = 0;
    for (size_t b = 0; b < q->vmm_count; b++) {
        if (q->vmm_released[b] || q->vmm_refs[b]) continue;
        uint64_t first = b * q->vmm_block_bytes / SLAB_SZ;
        uint64_t end = first + q->vmm_block_bytes / SLAB_SZ;
        if (end > g_nchunks) end = g_nchunks;
        for (uint64_t i = first; i < end; i++) {
            if (g_slabs[i].nobj || g_slabs[i].nfree) { error = EIO; goto out; }
            for (size_t k = 0; k < sizeof(g_slabs[i].bm); k++)
                if (g_slabs[i].bm[k]) { error = EIO; goto out; }
        }
        if (!cuda_vmm_pool_block_allocated(q->vmm_pool, b)) {
            q->vmm_released[b] = 1;
            continue;
        }
        candidate = 1;
    }
    if (!candidate) goto out;
    int result;
    do result = fsync(g_back_fd); while (result < 0 && errno == EINTR);
    if (result < 0) { error = errno; goto out; }
    uint64_t before = q->vmm_released_bytes;
    for (size_t b = 0; b < q->vmm_count; b++) {
        if (q->vmm_released[b] || q->vmm_refs[b]) continue;
        int cuda_result = cuda_vmm_pool_release_block(q->vmm_pool, b);
        if (cuda_result) {
            fprintf(stderr, "UBLK_VMM_RELEASE_FAILED block=%zu cuda=%d\n", b, cuda_result);
            error = EIO; goto out;
        }
        q->vmm_released[b] = 1;
        size_t bytes = g_vram_size - b * q->vmm_block_bytes;
        if (bytes > q->vmm_block_bytes) bytes = q->vmm_block_bytes;
        q->vmm_released_bytes += bytes;
    }
    if (!before && q->vmm_released_bytes && q->drain_cursor < g_npages) {
        printf("UBLK_VMM_EARLY_RELEASE bytes=%llu cursor=%llu pages=%llu\n",
               (unsigned long long)q->vmm_released_bytes,
               (unsigned long long)q->drain_cursor, (unsigned long long)g_npages);
        fflush(stdout);
    }
out:
    pthread_mutex_unlock(&g_alloc_lock);
    return error;
}

enum { DISK_RESERVE_OFF, DISK_RESERVE_READY, DISK_RESERVE_ATTEMPTED, DISK_RESERVE_CLEANED };
static const uint64_t disk_reserve_bytes = 64ULL * 1024 * 1024;

static int disk_space_at_least(int fd, uint64_t bytes, int *error)
{
    struct statvfs space;
    if (fstatvfs(fd, &space) != 0) { *error = errno; return -1; }
    if (!space.f_frsize) { *error = EIO; return -1; }
    uint64_t blocks = bytes / space.f_frsize + (bytes % space.f_frsize != 0);
    *error = 0;
    return space.f_bavail >= blocks;
}

static int init_disk_reserve(struct queue *q, const char *dir)
{
    struct statfs fs;
    struct stat backing_st, st;
    int error = 0, available;
    if (fstatfs(g_back_fd, &fs) != 0) {
        fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", errno);
        return 0;
    }
    if (fs.f_type != BTRFS_SUPER_MAGIC) {
        puts("UBLK_DISK_RESERVE_NOT_APPLICABLE");
        return 0;
    }
    if (fstat(g_back_fd, &backing_st) != 0) {
        fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", errno);
        return 0;
    }
    available = disk_space_at_least(g_back_fd, 2 * disk_reserve_bytes, &error);
    if (available < 0) {
        fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", error);
        return 0;
    }
    if (!available) {
        puts("UBLK_DISK_RESERVE_UNAVAILABLE reason=less_than_128MiB");
        return 0;
    }
    int fd = open(dir, O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = errno;
    } else if (fallocate(fd, 0, 0, (off_t)disk_reserve_bytes) != 0) {
        error = errno;
    } else if (fstat(fd, &st) != 0) {
        error = errno;
    } else if (!S_ISREG(st.st_mode) || st.st_nlink != 0 ||
               st.st_dev != backing_st.st_dev || st.st_blocks < (off_t)(disk_reserve_bytes / 512)) {
        error = EIO;
    }
    if (error) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", error);
        fd = -1;
    }
    if (fd >= 0) {
        available = disk_space_at_least(g_back_fd, disk_reserve_bytes, &error);
        if (available != 1) {
            close(fd);
            if (available < 0) {
                fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", error);
                available = disk_space_at_least(g_back_fd, disk_reserve_bytes, &error);
                if (available < 0) {
                    fprintf(stderr, "[nbd-vram] cannot verify Btrfs headroom after reserve attempt: errno=%d\n", error);
                    return -1;
                }
                if (!available) {
                    fprintf(stderr, "[nbd-vram] Btrfs backing requires at least 64 MiB available after reserve attempt\n");
                    return -1;
                }
                puts("UBLK_DISK_RESERVE_UNAVAILABLE reason=headroom_query_failed");
                return 0;
            }
            puts("UBLK_DISK_RESERVE_UNAVAILABLE reason=headroom_not_preserved");
            fd = -1;
        }
    }
    if (fd >= 0) {
        q->spare_fd = fd;
        q->reserve_state = DISK_RESERVE_READY;
        printf("UBLK_DISK_RESERVE_READY bytes=%llu\n",
               (unsigned long long)disk_reserve_bytes);
        fflush(stdout);
        return 0;
    }
    available = disk_space_at_least(g_back_fd, disk_reserve_bytes, &error);
    if (available < 0) {
        fprintf(stderr, "UBLK_DISK_RESERVE_UNAVAILABLE errno=%d\n", error);
        return -1;
    }
    if (!available) {
        fprintf(stderr, "[nbd-vram] Btrfs backing requires at least 64 MiB available after reserve attempt\n");
        return -1;
    }
    return 0;
}

static void check_disk_reserve(struct queue *q)
{
    int error = 0;
    if (q->reserve_state != DISK_RESERVE_READY || q->spare_fd < 0) return;
    int available = disk_space_at_least(g_back_fd, disk_reserve_bytes, &error);
    if (available < 0) {
        if (!q->reserve_query_error_logged) {
            fprintf(stderr, "UBLK_DISK_RESERVE_QUERY_FAILED errno=%d\n", error);
            q->reserve_query_error_logged = 1;
        }
        return;
    }
    if (available) return;
    int fd = q->spare_fd;
    q->spare_fd = -1;
    q->reserve_state = DISK_RESERVE_ATTEMPTED;
    int close_result = close(fd), close_error = errno;
    int sync_result = syncfs(g_back_fd), sync_error = errno;
    error = close_result < 0 ? close_error : sync_result < 0 ? sync_error : 0;
    printf("UBLK_DISK_RESERVE_%s errno=%d\n", error ? "FAILED" : "RELEASED", error);
    fflush(stdout);
}

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

/* The queue owns the payload; only its context-bound worker may release CUDA. */
static uint32_t drain_mock_pool(struct queue *q, int *finished)
{
    *finished = 0;
    if (!q->drain_cursor) {
        pthread_mutex_lock(&g_alloc_lock);
        g_pool_retired = 1;
        pthread_mutex_unlock(&g_alloc_lock);
    }
    if (g_compress != COMP_LZO || g_dedup || g_back_fd < 0 ||
        (q->backend == QUEUE_BACKEND_MOCK &&
         (_cuMemcpyHtoDAsync != mock_upload || _cuMemcpyDtoHAsync != mock_download ||
          _cuStreamSynchronize != mock_sync)) ||
        (q->backend == QUEUE_BACKEND_CUDA &&
         (!_cuMemFree || !_cuStreamSynchronize || !_cuMemcpyHtoDAsync ||
          !_cuMemcpyDtoHAsync || _cuMemcpyHtoDAsync == mock_upload ||
          _cuMemcpyDtoHAsync == mock_download || _cuStreamSynchronize == mock_sync)) ||
        (q->backend != QUEUE_BACKEND_MOCK && q->backend != QUEUE_BACKEND_CUDA) ||
        !q->drain_page ||
        (uintptr_t)q->drain_page % 4096 || !g_ptes || !g_slabs || !g_vram_ptr)
        return EOPNOTSUPP;

    uint32_t ref_error = vmm_prepare_refs(q);
    if (ref_error) return ref_error;

    uint64_t end = q->drain_cursor + 64;
    if (end > g_npages) end = g_npages;
    for (; q->drain_cursor < end; q->drain_cursor++) {
        uint64_t pg = q->drain_cursor;
        uint32_t error = 0;
        lock_page(pg);
        uint8_t kind = g_ptes[pg].kind;
        if (kind == PTE_RAW || kind == PTE_COMPRESSED) {
            error = load_plain(pg, q->drain_page, NULL);
            if (!error) {
                struct cpend op = { .pg = pg, .plain = q->drain_page };
                error = disk_replace(&op);
            }
        } else if (kind != PTE_NONE && kind != PTE_SAME &&
                   (kind != PTE_DISK || !valid_disk_entry(pg, g_ptes[pg]))) {
            error = EIO;
        }
        unlock_page(pg);
        if (error) return error;
    }
    uint32_t release_error = vmm_release_empty(q);
    if (release_error) return release_error;
    if (q->drain_cursor < g_npages) return 0;
    int result;
    do result = fsync(g_back_fd); while (result < 0 && errno == EINTR);
    if (result < 0) return errno;
    for (uint64_t pg = 0; pg < g_npages; pg++) {
        lock_page(pg);
        uint8_t kind = g_ptes[pg].kind;
        int safe = kind == PTE_NONE || kind == PTE_SAME ||
                   (kind == PTE_DISK && valid_disk_entry(pg, g_ptes[pg]));
        unlock_page(pg);
        if (!safe) return EIO;
    }
    pthread_mutex_lock(&g_alloc_lock);
    int live = g_vram_obj_bytes != 0 || g_nfree_chunks != g_nchunks;
    for (uint32_t i = 0; i < g_nchunks && !live; i++) {
        live = g_slabs[i].nobj != 0 || g_slabs[i].nfree != 0;
        for (size_t b = 0; b < sizeof(g_slabs[i].bm) && !live; b++)
            live = g_slabs[i].bm[b] != 0;
    }
    if (live) { pthread_mutex_unlock(&g_alloc_lock); return EBUSY; }
    if (q->backend == QUEUE_BACKEND_CUDA) {
        if (_cuStreamSynchronize(NULL) != CUDA_SUCCESS ||
            (!q->vmm_enabled && _cuMemFree(g_vram_ptr) != CUDA_SUCCESS)) {
            pthread_mutex_unlock(&g_alloc_lock);
            return EIO;
        }
        if (q->vmm_enabled) {
            for (size_t b = 0; b < q->vmm_count; ++b) {
                if (cuda_vmm_pool_block_allocated(q->vmm_pool, b)) {
                    pthread_mutex_unlock(&g_alloc_lock); return EBUSY;
                }
            }
            /* Keep only VA and empty slab metadata; no physical blocks remain. */
            pthread_mutex_unlock(&g_alloc_lock);
            *finished = 1;
            return 0;
        }
    } else {
        free((void *)(uintptr_t)g_vram_ptr);
    }
    g_vram_ptr = 0; g_vram_size = 0;
    free(g_slabs); g_slabs = NULL;
    for (int k = 0; k < NCLASS; k++) g_class_head[k] = SLAB_NONE;
    g_free_chunk = SLAB_NONE; g_nchunks = g_nfree_chunks = 0;
    g_vram_obj_bytes = 0;
    pthread_mutex_unlock(&g_alloc_lock);
    *finished = 1;
    return 0;
}

static int cuda_headroom(size_t pending);
#define VMM_REARM_MARGIN (64u * 1024u * 1024u)
static uint32_t vmm_rearm_pool(struct queue *q)
{
    if (q->reserve_state == DISK_RESERVE_ATTEMPTED) return ENOSPC;
    if (!q->vmm_enabled || !q->vmm_pool || !q->vmm_refs || !q->vmm_released ||
        !q->vmm_count || !g_vram_ptr || !g_slabs || !g_nchunks ||
        !g_pool_retired || !q->vmm_refs_ready || q->vmm_ref_error)
        return EIO;
    if (cuda_headroom(VMM_REARM_MARGIN)) return EAGAIN;
    pthread_mutex_lock(&g_alloc_lock);
    uint32_t error = EIO;
    if (g_vram_obj_bytes || g_nfree_chunks != g_nchunks) goto out;
    for (size_t b = 0; b < q->vmm_count; ++b)
        if (q->vmm_refs[b] || !q->vmm_released[b] ||
            cuda_vmm_pool_block_allocated(q->vmm_pool, b)) goto out;
    for (uint32_t i = 0; i < g_nchunks; ++i) {
        if (g_slabs[i].nobj || g_slabs[i].nfree) goto out;
        for (size_t b = 0; b < sizeof(g_slabs[i].bm); ++b)
            if (g_slabs[i].bm[b]) goto out;
    }
    for (int k = 0; k < NCLASS; ++k) g_class_head[k] = SLAB_NONE;
    g_free_chunk = SLAB_NONE;
    for (uint32_t i = g_nchunks; i-- > 0;) {
        g_slabs[i].chunk = i; g_slabs[i].next = g_free_chunk; g_free_chunk = i;
    }
    memset(q->vmm_refs, 0, q->vmm_count * sizeof(*q->vmm_refs));
    memset(q->vmm_released, 0, q->vmm_count * sizeof(*q->vmm_released));
    q->vmm_refs_ready = q->vmm_ref_error = 0; q->vmm_released_bytes = 0;
    q->drain_cursor = 0; q->drain_jobs_left = 0;
    g_retired_slot_freed = NULL; vmm_draining_queue = NULL;
    g_pool_retired = 0;
    error = 0;
out:
    pthread_mutex_unlock(&g_alloc_lock);
    return error;
}

static void *worker(void *opaque)
{
    struct queue *q = opaque;
    int startup = 1;
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0) {
        startup = -errno;
        perror("ublk worker PR_SET_IO_FLUSHER");
    }
    if (startup > 0 && q->backend == QUEUE_BACKEND_CUDA &&
        _cuCtxSetCurrent(g_cu_ctx) != CUDA_SUCCESS) {
        fprintf(stderr, "ublk worker CUDA context bind failed\n");
        startup = -EIO;
    }
    pthread_mutex_lock(&q->lock);
    q->started = startup; pthread_cond_broadcast(&q->cond);
    if (startup < 0) { pthread_mutex_unlock(&q->lock); return NULL; }
    t_cstage = q->stage; t_lzo_work = g_lzo_work[0];
    for (;;) {
        if (q->rearm_pending) {
            q->rearm_pending = 0;
            if (q->drain_state == DRAIN_OK && !q->shutdown_requested && !q->stopping) {
                q->drain_state = DRAIN_REARMING;
                pthread_mutex_unlock(&q->lock);
                uint32_t error = vmm_rearm_pool(q);
                pthread_mutex_lock(&q->lock);
                if (!error && q->shutdown_requested) g_pool_retired = 1;
                q->drain_error = error == EAGAIN ? 0 : error;
                if (error && error != EAGAIN) q->drain_state = DRAIN_FAILED;
                else if (error || q->shutdown_requested) q->drain_state = DRAIN_OK;
                else q->drain_state = DRAIN_IDLE;
                pthread_cond_broadcast(&q->cond);
                int shutdown = q->shutdown_requested;
                pthread_mutex_unlock(&q->lock);
                if (!error && !shutdown) puts("UBLK_VMM_REARMED physical_bytes=0");
                else fprintf(stderr, "UBLK_VMM_REARM_DEFERRED errno=%u shutdown=%d\n", error, shutdown);
                pthread_mutex_lock(&q->lock);
                continue;
            }
        }
        struct job *j = NULL;
        for (int i = 0; i < QDEPTH; i++)
            if (q->jobs[i].state == JOB_QUEUED && (!j || q->jobs[i].seq < j->seq)) j = &q->jobs[i];
        int starting_drain = q->drain_state == DRAIN_PENDING && (!j || j->seq > q->drain_seq);
        if (starting_drain)
            q->drain_state = DRAIN_RUNNING;
        if (q->drain_state == DRAIN_RUNNING && (!j || !q->drain_jobs_left)) {
            pthread_mutex_unlock(&q->lock);
            if (starting_drain) {
                printf("UBLK_DRAIN_STARTED seq=%llu\n", (unsigned long long)q->drain_seq);
                fflush(stdout);
            }
            check_disk_reserve(q);
            int finished;
            uint32_t error = drain_mock_pool(q, &finished);
            pthread_mutex_lock(&q->lock);
            q->drain_jobs_left = QDEPTH;
            if (!error && !finished) continue;
            q->drain_error = error;
            q->drain_state = error ? DRAIN_FAILED : DRAIN_OK;
            pthread_cond_broadcast(&q->cond);
            pthread_mutex_unlock(&q->lock);
            printf("UBLK_DRAIN_%s seq=%llu errno=%u\n", error ? "FAILED" : "OK",
                   (unsigned long long)q->drain_seq, error);
            fflush(stdout);
            pthread_mutex_lock(&q->lock);
            continue;
        }
        if (!j && q->stopping) break;
        if (!j) { pthread_cond_wait(&q->cond, &q->lock); continue; }
        if (q->drain_state == DRAIN_RUNNING && q->drain_jobs_left)
            --q->drain_jobs_left;
        j->state = JOB_WORKING;
        pthread_mutex_unlock(&q->lock);
        check_disk_reserve(q);
        uint32_t err = j->op == UBLK_IO_OP_WRITE ? comp_write(j->off, j->data, COMP_PAGE, NULL) :
                                                   comp_read(j->off, j->data, COMP_PAGE, NULL);
        if (err)
            fprintf(stderr, "UBLK_STORE_ERROR op=%d offset=%llu errno=%u\n",
                    j->op, (unsigned long long)j->off, err);
        if (trace_io && !err && g_ptes[j->off / COMP_PAGE].kind == PTE_DISK) {
            printf("UBLK_BACKING_IO op=%s page=%llu\n",
                   j->op == UBLK_IO_OP_WRITE ? "write" : "read",
                   (unsigned long long)(j->off / COMP_PAGE));
            fflush(stdout);
        }
        if (trace_io && !err && g_ptes[j->off / COMP_PAGE].kind == PTE_COMPRESSED) {
            printf("UBLK_LZO_IO op=%s\n", j->op == UBLK_IO_OP_WRITE ? "write" : "read");
            fflush(stdout);
        }
        pthread_mutex_lock(&q->lock);
        j->result = err ? -(int)err : COMP_PAGE;
        j->state = JOB_DONE;
        if (q->ublk && ublksrv_queue_send_event(q->ublk) < 0) {
            fprintf(stderr, "ublk event notification failed\n");
            _Exit(1);
        }
        pthread_cond_broadcast(&q->cond);
    }
    pthread_mutex_unlock(&q->lock); t_cstage = NULL; t_lzo_work = NULL; return NULL;
}

/* Single queue-owner contract. Successful submissions copy the whole WRITE;
 * READ output remains in the job until the owner consumes completion. */
static int submit(struct queue *q, unsigned tag, int op, unsigned flags, uint64_t off, const void *in)
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
    j->tag = tag; j->op = op; j->off = off; j->seq = q->next_seq++;
    if (op == UBLK_IO_OP_WRITE) memcpy(j->data, in, COMP_PAGE);
    j->state = JOB_QUEUED; pthread_cond_broadcast(&q->cond); pthread_mutex_unlock(&q->lock); return 0;
}

#include <getopt.h>
#include <limits.h>

static struct queue workq = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                              .spare_fd = -1 };
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t init_cond = PTHREAD_COND_INITIALIZER;
static int queue_status;
static CUresult (*_cuMemGetInfo)(size_t *, size_t *);

#define GPU_PAYLOAD_DEFAULT (2ULL * 1024 * 1024)
/* Experimental payload cap, not a production or total-driver-memory guarantee. */
#define GPU_PAYLOAD_MAX (16ULL * 1024 * 1024 * 1024)

static size_t vram_reserve(size_t total)
{
    size_t reserve = total / 8;
    size_t cap = (size_t)2 * 1024 * 1024 * 1024;
    return reserve < cap ? reserve : cap;
}

static int vmm_prepare_slot(uint64_t offset);
static CUresult allocate_cuda_pool(struct queue *q, CUdevice device)
{
    if (!q->vmm_enabled) return _cuMemAlloc(&g_vram_ptr, g_vram_size);
    uint64_t base = 0;
    int result = cuda_vmm_pool_reserve(&q->vmm_pool, g_libcuda, device, g_vram_size, &base);
    if (result) return result;
    g_vram_ptr = base;
    q->vmm_block_bytes = cuda_vmm_pool_block_size(q->vmm_pool);
    q->vmm_count = cuda_vmm_pool_block_count(q->vmm_pool);
    if (!q->vmm_count || !q->vmm_block_bytes || q->vmm_block_bytes % SLAB_SZ) return 1;
    q->vmm_refs = calloc(q->vmm_count, sizeof(*q->vmm_refs));
    q->vmm_released = calloc(q->vmm_count, sizeof(*q->vmm_released));
    if (!q->vmm_refs || !q->vmm_released) return 2; /* CUDA_ERROR_OUT_OF_MEMORY */
    g_prepare_slot = vmm_prepare_slot;
    printf("UBLK_VMM_READY blocks=%zu block_bytes=%zu lazy=1\n", q->vmm_count, q->vmm_block_bytes);
    return CUDA_SUCCESS;
}

static CUresult free_cuda_pool(struct queue *q)
{
    int result;
    if (q->vmm_pool) result = cuda_vmm_pool_destroy(&q->vmm_pool);
    else if (q->vmm_enabled) result = g_vram_ptr ? 1 : CUDA_SUCCESS;
    else result = g_vram_ptr ? _cuMemFree(g_vram_ptr) : CUDA_SUCCESS;
    if (!result) g_vram_ptr = 0;
    return result;
}

/* A snapshot cannot reserve memory against other GPU clients. */
static int cuda_headroom(size_t pending)
{
    size_t available = 0, total = 0;
    if (!_cuMemGetInfo ||
        _cuMemGetInfo(&available, &total) != CUDA_SUCCESS ||
        total < 8 || available > total) {
        fprintf(stderr, "ublk CUDA memory query invalid or unavailable\n");
        return -1;
    }
    size_t reserve = vram_reserve(total);
    if (available < reserve || pending > available - reserve) {
        fprintf(stderr,
                "ublk CUDA headroom rejected: pending=%zu free=%zu reserve=%zu\n",
                pending, available, reserve);
        return -1;
    }
    return 0;
}

static int vmm_prepare_slot(uint64_t offset)
{
    struct queue *q = &workq;
    if (!q->vmm_pool || !q->vmm_block_bytes || offset >= g_vram_size) return EIO;
    size_t block = offset / q->vmm_block_bytes;
    if (block >= q->vmm_count) return EIO;
    if (!cuda_vmm_pool_block_allocated(q->vmm_pool, block)) {
        size_t bytes = g_vram_size - block * q->vmm_block_bytes;
        if (bytes > q->vmm_block_bytes) bytes = q->vmm_block_bytes;
        if (cuda_headroom(bytes)) return ENOSPC;
    }
    int error = cuda_vmm_pool_commit_block(q->vmm_pool, block);
    if (error) fprintf(stderr, "UBLK_VMM_COMMIT_FAILED block=%zu cuda=%d\n", block, error);
    return error ? EIO : 0;
}

static void request_drain(const char *reason, size_t available, size_t total, size_t threshold)
{
    int requested = 0, state;
    uint64_t seq;
    uint32_t error;
    pthread_mutex_lock(&workq.lock);
    if (workq.drain_state == DRAIN_IDLE && !workq.stopping &&
        workq.next_seq != UINT64_MAX) {
        workq.drain_seq = workq.next_seq++;
        workq.drain_cursor = 0;
        workq.drain_jobs_left = 0;
        workq.drain_state = DRAIN_PENDING;
        requested = 1;
        pthread_cond_broadcast(&workq.cond);
    }
    state = workq.drain_state; seq = workq.drain_seq;
    error = state == DRAIN_IDLE ? EOVERFLOW : workq.drain_error;
    pthread_mutex_unlock(&workq.lock);
    if (requested) {
        if (reason)
            printf("UBLK_AUTO_DRAIN reason=%s free=%zu total=%zu threshold=%zu\n",
                   reason, available, total, threshold);
        printf("UBLK_DRAIN_REQUESTED seq=%llu\n", (unsigned long long)seq);
    } else {
        printf("UBLK_DRAIN_STATUS state=%d seq=%llu errno=%u\n",
               state, (unsigned long long)seq, error);
    }
    fflush(stdout);
}

/* Main retains the context made current by cuCtxCreate; only the worker drains. */
static void poll_free_vram(void)
{
    pthread_mutex_lock(&workq.lock);
    int state = workq.drain_state;
    pthread_mutex_unlock(&workq.lock);
    if (state != DRAIN_IDLE && state != DRAIN_OK) return;
    size_t available = 0, total = 0;
    CUresult result = _cuMemGetInfo ? _cuMemGetInfo(&available, &total) : -1;
    size_t threshold = vram_reserve(total);
    if (result != CUDA_SUCCESS || total < 8 || available > total) {
        if (state != DRAIN_IDLE) return;
        fprintf(stderr, "UBLK_AUTO_DRAIN_QUERY_FAILED cuda_result=%d free=%zu total=%zu\n",
                result, available, total);
        request_drain("query_failed", available, total, threshold);
    } else {
        if (test_low_free && access("/tmp/cuda-low-free", F_OK) == 0) {
            size_t actual_free = available;
            available = threshold - 1;
            fprintf(stderr, "UBLK_TEST_PRESSURE synthetic=1 actual_free=%zu reported_free=%zu total=%zu\n",
                    actual_free, available, total);
        }
        if (state == DRAIN_OK) {
            if (available >= threshold && available - threshold >= VMM_REARM_MARGIN) {
                pthread_mutex_lock(&workq.lock);
                if (workq.drain_state == DRAIN_OK && workq.vmm_enabled && workq.vmm_pool &&
                    !workq.shutdown_requested && !workq.stopping && !workq.rearm_pending) {
                    workq.rearm_pending = 1;
                    pthread_cond_broadcast(&workq.cond);
                }
                pthread_mutex_unlock(&workq.lock);
            }
        } else if (available < threshold)
            request_drain("low_free", available, total, threshold);
    }
}

/* A failed control command does not establish that kernel users are detached. */
static int swap_table_empty(void);
#ifndef UBLK_U_CMD_TRY_STOP_DEV
#define UBLK_U_CMD_TRY_STOP_DEV _IOWR('u', 0x17, struct ublksrv_ctrl_cmd)
#endif
static int kernel_safe_stop, safe_ctrl_fd = -1, safe_ring_ready, safe_control_broken;
static int managed_swap, managed_swap_active;
static char managed_swap_path[64];
static int safe_device_id = -1;
static struct io_uring safe_ring;

static void managed_swap_tick(void)
{
    pthread_mutex_lock(&workq.lock);
    int polling = workq.backend == QUEUE_BACKEND_CUDA && workq.drain_state == DRAIN_IDLE;
    pthread_mutex_unlock(&workq.lock);
    if (polling) poll_free_vram();
}

static int managed_swap_start(int id)
{
    snprintf(managed_swap_path, sizeof(managed_swap_path), "/dev/ublkb%d", id);
    char *format[] = { "/usr/bin/mkswap", "--", managed_swap_path, NULL };
    char *activate[] = { "/usr/bin/swapon", "--priority", "90", "--", managed_swap_path, NULL };
    /* Main owns this device's lifetime; STOP is not submitted while a command runs. */
    if (ublk_swap_command(format, managed_swap_tick) != 0) return -1;
    /* Even a failed utility exit may leave activation uncertain: always evacuate. */
    managed_swap_active = 1;
    if (ublk_swap_command(activate, managed_swap_tick) != 0) return -1;
    puts("UBLK_MANAGED_SWAP_ACTIVE"); fflush(stdout);
    return 0;
}

static void managed_swap_stop(void)
{
    if (!managed_swap_active) return;
    char *evacuate[] = { "/usr/bin/swapoff", "--", managed_swap_path, NULL };
    if (ublk_swap_command(evacuate, managed_swap_tick) == 0) {
        managed_swap_active = 0;
        puts("UBLK_MANAGED_SWAP_OFF"); fflush(stdout);
    } else fprintf(stderr, "UBLK_MANAGED_SWAPOFF_FAILED resources_retained_until_safe_stop=1\n");
}

static int kernel_try_stop(void)
{
    if (!safe_ring_ready || safe_control_broken || safe_device_id < 0) return -EIO;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&safe_ring);
    if (!sqe) return -EIO;
    memset(sqe, 0, sizeof(*sqe));
    io_uring_prep_uring_cmd(sqe, IORING_OP_URING_CMD, safe_ctrl_fd);
    sqe->cmd_op = UBLK_U_CMD_TRY_STOP_DEV;
    struct ublksrv_ctrl_cmd cmd = { .dev_id = safe_device_id, .queue_id = UINT16_MAX };
    memcpy(sqe->cmd, &cmd, sizeof(cmd));
    if (io_uring_submit(&safe_ring) != 1) { safe_control_broken = 1; return -EIO; }
    struct io_uring_cqe *cqe;
    int error;
    do { error = io_uring_wait_cqe(&safe_ring, &cqe); } while (error == -EINTR);
    if (error < 0) { safe_control_broken = 1; return error; }
    int result = cqe->res;
    io_uring_cqe_seen(&safe_ring, cqe);
    return result;
}

static int stop_device_confirmed(struct ublksrv_ctrl_dev *ctrl, const sigset_t *signals)
{
    pthread_mutex_lock(&workq.lock);
    workq.shutdown_requested = 1; workq.rearm_pending = 0;
    pthread_mutex_unlock(&workq.lock);
    int attempt = kernel_safe_stop || swap_table_empty();
    for (;;) {
        if (attempt) {
            managed_swap_stop();
            puts("UBLK_STOP_BEGIN"); fflush(stdout);
            int result = kernel_safe_stop ? kernel_try_stop() : ublksrv_ctrl_stop_dev(ctrl);
            if (result >= 0) {
                puts("UBLK_STOP_DONE"); fflush(stdout);
                return 0;
            }
            fprintf(stderr, "UBLK_STOP_RETAINED result=%d resources_retained=1\n", result);
        } else {
            fprintf(stderr, "UBLK_STOP_DEFERRED swap active or state unavailable resources_retained=1\n");
        }
        attempt = 0;
        int sig = 0, error = 0;
        pthread_mutex_lock(&workq.lock);
        int polling = workq.backend == QUEUE_BACKEND_CUDA && workq.drain_state == DRAIN_IDLE;
        pthread_mutex_unlock(&workq.lock);
        if (polling) {
            poll_free_vram();
            const struct timespec timeout = { .tv_sec = 0, .tv_nsec = 250000000 };
            sig = sigtimedwait(signals, NULL, &timeout);
            if (sig < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                error = errno;
            }
        } else error = sigwait(signals, &sig);
        if (error) {
            fprintf(stderr, "UBLK_STOP_WAIT_ERROR error=%d resources_retained=1\n", error);
            const struct timespec pause = { .tv_sec = 0, .tv_nsec = 250000000 };
            nanosleep(&pause, NULL);
            continue;
        }
        if (sig == SIGUSR2) request_drain(NULL, 0, 0, 0);
        else if (sig == SIGTERM || sig == SIGINT) attempt = kernel_safe_stop || swap_table_empty();
        else fprintf(stderr, "UBLK_STOP_SIGNAL_RETAINED signal=%d resources_retained=1\n", sig);
    }
}

static int target_init(struct ublksrv_dev *dev, int type, int argc, char **argv)
{
    (void)type; (void)argc; (void)argv;
    dev->tgt.dev_size = g_export_size;
    dev->tgt.tgt_ring_depth = QDEPTH;
    dev->tgt.nr_fds = 0;
    return 0;
}

static int target_io(const struct ublksrv_queue *q, const struct ublk_io_data *io)
{
    const struct ublksrv_io_desc *d = io->iod;
    unsigned op = ublksrv_get_op(d);
    uint64_t off = d->start_sector <= UINT64_MAX / 512 ? d->start_sector * 512 : UINT64_MAX;
    unsigned flags = d->op_flags & ~0xffU;
    int result;
    void *buf = ublksrv_queue_get_io_buf(q, io->tag);
    if (op != UBLK_IO_OP_READ && op != UBLK_IO_OP_WRITE)
        result = -EOPNOTSUPP;
    /* Fail-fast flags request no retries; this target already returns I/O errors directly. */
    else if (flags & ~(UBLK_IO_F_SWAP | UBLK_IO_F_FAILFAST_DEV |
                       UBLK_IO_F_FAILFAST_TRANSPORT | UBLK_IO_F_FAILFAST_DRIVER))
        result = -EOPNOTSUPP;
    else if (d->nr_sectors != COMP_PAGE / 512 || off % COMP_PAGE ||
             off >= g_export_size || !buf)
        result = -EINVAL;
    else {
        result = submit(&workq, io->tag, op, 0, off, buf);
        if (!result) return 0;
    }
    if (result < 0)
        fprintf(stderr, "UBLK_REQUEST_REJECTED op=%u flags=%u sectors=%u offset=%llu result=%d\n",
                op, flags, d->nr_sectors, (unsigned long long)off, result);
    ublksrv_complete_io(q, io->tag, result);
    return 0;
}

static void target_event(const struct ublksrv_queue *q)
{
    pthread_mutex_lock(&workq.lock);
    ublksrv_queue_handled_event(q);
    for (int i = 0; i < QDEPTH; i++) {
        struct job *j = &workq.jobs[i];
        if (j->state != JOB_DONE) continue;
        if (j->result == COMP_PAGE && j->op == UBLK_IO_OP_READ)
            memcpy(ublksrv_queue_get_io_buf(q, j->tag), j->data, COMP_PAGE);
        ublksrv_complete_io(q, j->tag, j->result);
        j->state = JOB_FREE;
    }
    pthread_cond_broadcast(&workq.cond);
    pthread_mutex_unlock(&workq.lock);
}

static const struct ublksrv_tgt_type target_ops = {
    .name = "lzo", .init_tgt = target_init, .handle_io_async = target_io, .handle_event = target_event,
};

static void *queue_thread(void *arg)
{
    const struct ublksrv_dev *dev = arg;
    const struct ublksrv_queue *q;
    workq.owner = pthread_self();
    q = ublksrv_queue_init(dev, 0, NULL);
    pthread_mutex_lock(&workq.lock); workq.ublk = q; pthread_mutex_unlock(&workq.lock);
    pthread_mutex_lock(&init_lock);
    queue_status = q ? 1 : -1;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    if (!q) return NULL;
    while (ublksrv_process_io(q) >= 0) {}
    pthread_mutex_lock(&workq.lock);
    workq.ublk = NULL; workq.queue_exited = 1;
    pthread_cond_broadcast(&workq.cond);
    pthread_mutex_unlock(&workq.lock);
    ublksrv_queue_deinit(q);
    kill(getpid(), SIGUSR1);
    return NULL;
}

static int swap_table_empty(void)
{
    FILE *f = fopen("/proc/swaps", "r");
    char header[256];
    if (!f) return 0;
    int empty = fgets(header, sizeof(header), f) &&
        !strcmp(header, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n") &&
        fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) empty = 0;
    return empty;
}

static int number(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long n;
    if (!*s || *s == '-') return -1;
    errno = 0; n = strtoull(s, &end, 10);
    if (errno || *end) return -1;
    *out = n; return 0;
}

int main(int argc, char **argv)
{
    struct ublksrv_ctrl_dev *ctrl = NULL;
    const struct ublksrv_dev *dev = NULL;
    struct ublksrv_dev_data data = { .dev_id = -1, .max_io_buf_bytes = COMP_PAGE,
        .nr_hw_queues = 1, .queue_depth = QDEPTH, .ublksrv_flags = UBLKSRV_F_NEED_EVENTFD, .tgt_type = "lzo", .tgt_ops = &target_ops };
    struct ublk_params params = { .types = UBLK_PARAM_TYPE_BASIC,
        .basic = { .logical_bs_shift = 12, .physical_bs_shift = 12,
                   .io_opt_shift = 12, .io_min_shift = 12, .max_sectors = COMP_PAGE / 512 } };
    static const struct option opts[] = { {"device", required_argument, NULL, 'd'},
        {"size", required_argument, NULL, 's'}, {"backing", required_argument, NULL, 'b'},
        {"cuda", no_argument, NULL, 'c'},
        {"kernel-safe-stop", no_argument, NULL, 'k'},
        {"managed-swap", no_argument, NULL, 'm'},
        {"cuda-vmm", no_argument, NULL, 'v'},
        {"host", no_argument, NULL, 'h'},
        {"gpu-payload-bytes", required_argument, NULL, 'p'}, {NULL,0,NULL,0} };
    const char *backing = NULL;
    uint64_t n, size = 0, payload = GPU_PAYLOAD_DEFAULT;
    int payload_set = 0, host_mode = 0;
    int opt, rc = 1, added = 0, worker_started = 0, queue_started = 0, start_attempted = 0, sig;
    pthread_t qt;
    sigset_t signals;
    while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        if (opt == 'd' && !number(optarg, &n) && n <= INT_MAX) data.dev_id = (int)n;
        else if (opt == 's' && !number(optarg, &size)) {}
        else if (opt == 'b') backing = optarg;
        else if (opt == 'c') workq.backend = QUEUE_BACKEND_CUDA;
        else if (opt == 'k') kernel_safe_stop = 1;
        else if (opt == 'm') managed_swap = 1;
        else if (opt == 'v') workq.vmm_enabled = 1;
        else if (opt == 'h') host_mode = 1;
        else if (opt == 'p') {
            if (payload_set || !*optarg ||
                strspn(optarg, "0123456789") != strlen(optarg) ||
                number(optarg, &payload)) goto usage;
            payload_set = 1;
        }
        else goto usage;
    }
    if (optind != argc || data.dev_id < 0 || !backing || !size || size % COMP_PAGE || size > INT64_MAX / 2) goto usage;
    if (managed_swap && !kernel_safe_stop) goto usage;
    if (workq.vmm_enabled && workq.backend != QUEUE_BACKEND_CUDA) goto usage;
    if ((payload_set && workq.backend != QUEUE_BACKEND_CUDA) ||
        payload < SLAB_SZ || payload > GPU_PAYLOAD_MAX ||
        payload > SIZE_MAX || payload % SLAB_SZ) goto usage;
    if (host_mode) {
        uint64_t pages = size / COMP_PAGE, chunks = payload / SLAB_SZ;
        if (pages > UINT64_MAX / sizeof(*g_ptes) || chunks > UINT64_MAX / sizeof(*g_slabs)) goto usage;
        uint64_t page_bytes = pages * sizeof(*g_ptes), slab_bytes = chunks * sizeof(*g_slabs);
        if (page_bytes > UINT64_MAX - slab_bytes ||
            !ublk_host_admit(workq.backend == QUEUE_BACKEND_CUDA, kernel_safe_stop, page_bytes + slab_bytes)) {
            fprintf(stderr, "ublk host admission rejected: root, CUDA, safe-stop, clean test environment and RAM budget required\n");
            return 1;
        }
    } else if (ublk_guest_guard()) {
        fprintf(stderr, "ublk guest guard rejected environment\n"); return 1;
    }
    test_low_free = getenv("VRAM_TEST_CUDA_LOW_FREE") &&
                    !strcmp(getenv("VRAM_TEST_CUDA_LOW_FREE"), "after-marker");
    /* Preserve complete diagnostic lines when the VM supervisor captures a pipe. */
    if (setvbuf(stdout, NULL, _IOLBF, 0)) { fprintf(stderr, "ublk stdout buffering failed\n"); return 1; }
    if (host_mode) puts("UBLK_HOST_ADMITTED startup_budget_only=1");
    trace_io = getenv("VRAM_TEST_TRACE_IO") && !strcmp(getenv("VRAM_TEST_TRACE_IO"), "1");
    if (kernel_safe_stop) {
        struct io_uring_params ring_params = { .flags = IORING_SETUP_SQE128 };
        safe_ctrl_fd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (safe_ctrl_fd < 0 || io_uring_queue_init_params(2, &safe_ring, &ring_params) < 0) {
            if (safe_ctrl_fd >= 0) close(safe_ctrl_fd);
            fprintf(stderr, "ublk safe control initialization failed\n"); return 1;
        }
        safe_ring_ready = 1;
        safe_device_id = data.dev_id;
    }
    if (workq.backend == QUEUE_BACKEND_MOCK && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("ublk mlockall"); return 1;
    }
    sigemptyset(&signals); sigaddset(&signals, SIGTERM); sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGUSR1); sigaddset(&signals, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL)) { fprintf(stderr, "signal setup failed\n"); return 1; }
    g_export_size = size; g_vram_size = SLAB_SZ; g_nbd_threads = 1; g_compress = COMP_LZO; g_dedup = 0;
    if (workq.backend == QUEUE_BACKEND_CUDA) {
        CUdevice device;
        g_vram_size = payload;
        /* Host admission uses the installed driver, never a fixture search path. */
        if (host_mode ? load_libcuda_from("/usr/lib/libcuda.so.1") : load_libcuda()) goto out;
        _cuMemGetInfo = dlsym(g_libcuda, "cuMemGetInfo_v2");
        if (_cuInit(0) != CUDA_SUCCESS || _cuDeviceGet(&device, 0) != CUDA_SUCCESS ||
            _cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_BLOCKING_SYNC, device) != CUDA_SUCCESS ||
            cuda_headroom(workq.vmm_enabled ? 0 : (size_t)g_vram_size) ||
            allocate_cuda_pool(&workq, device) != CUDA_SUCCESS ||
            _cuMemAllocHost((void **)&workq.stage, compression_stage_size()) != CUDA_SUCCESS ||
            cuda_headroom(0)) {
            fprintf(stderr, "ublk CUDA initialization failed (no mock fallback)\n");
            goto out;
        }
        /* Initialize the driver's VA mappings before enabling future locking. */
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            perror("ublk mlockall"); goto out;
        }
    } else {
        g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
        _cuMemcpyHtoDAsync = mock_upload; _cuMemcpyDtoHAsync = mock_download; _cuStreamSynchronize = mock_sync;
    }
    if (!g_vram_ptr || compress_init() || backing_init(backing)) goto out;
    if (host_mode) {
        struct statfs fs;
        if (fstatfs(g_back_fd, &fs) != 0 || fs.f_type != BTRFS_SUPER_MAGIC) {
            fprintf(stderr, "ublk host mode requires verified Btrfs backing\n"); goto out;
        }
    }
    if (init_disk_reserve(&workq, backing)) goto out;
    if (host_mode && workq.reserve_state != DISK_RESERVE_READY) {
        fprintf(stderr, "ublk host mode requires emergency disk reserve\n"); goto out;
    }
    if (workq.backend == QUEUE_BACKEND_MOCK) workq.stage = calloc(1, compression_stage_size());
    if (posix_memalign((void **)&workq.drain_page, 4096, COMP_PAGE)) goto out;
    memset(workq.drain_page, 0, COMP_PAGE);
    if (workq.backend == QUEUE_BACKEND_CUDA && cuda_headroom(0)) goto out;
    if (!workq.stage || pthread_create(&workq.worker, NULL, worker, &workq)) goto out;
    worker_started = 1;
    pthread_mutex_lock(&workq.lock);
    while (!workq.started) pthread_cond_wait(&workq.cond, &workq.lock);
    int startup = workq.started;
    pthread_mutex_unlock(&workq.lock);
    if (startup < 0) { fprintf(stderr, "ublk worker startup failed: %d\n", startup); goto out; }
    data.run_dir = ublksrv_get_pid_dir();
    ctrl = ublksrv_ctrl_init(&data);
    if (!ctrl) { fprintf(stderr, "ublksrv_ctrl_init failed\n"); goto out; }
    if (kernel_safe_stop) {
        __u64 features = 0;
        if (ublksrv_ctrl_get_features(ctrl, &features) < 0 || !(features & (1ULL << 17))) {
            fprintf(stderr, "UBLK_SAFE_STOP_UNSUPPORTED no_fallback=1\n"); goto out;
        }
        puts("UBLK_SAFE_STOP_ENABLED kernel_openers=1 no_fallback=1");
    }
    if (ublksrv_ctrl_add_dev(ctrl) < 0) { fprintf(stderr, "ublk add failed\n"); goto out; }
    added = 1;
    {
        char path[64];
        struct stat st;
        snprintf(path, sizeof(path), "/dev/ublkc%d", data.dev_id);
        for (int i = 0; i < 100 && stat(path, &st) != 0 && errno == ENOENT; i++) usleep(10000);
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
            fprintf(stderr, "ublk control device unavailable: %s\n", path); goto out;
        }
    }
    dev = ublksrv_dev_init(ctrl);
    if (!dev) { fprintf(stderr, "ublk device init failed\n"); goto out; }
    if (pthread_create(&qt, NULL, queue_thread, (void *)dev)) goto out;
    queue_started = 1;
    pthread_mutex_lock(&init_lock);
    while (!queue_status) pthread_cond_wait(&init_cond, &init_lock);
    pthread_mutex_unlock(&init_lock);
    if (queue_status < 0) { fprintf(stderr, "ublk queue init failed\n"); goto out; }
    params.basic.dev_sectors = size / 512;
    if (ublksrv_ctrl_set_params(ctrl, &params) < 0) {
        fprintf(stderr, "ublk start failed\n"); goto out;
    }
    start_attempted = 1;
    if (ublksrv_ctrl_start_dev(ctrl, getpid()) < 0) {
        fprintf(stderr, "ublk start failed\n"); goto out;
    }
    if (managed_swap && managed_swap_start(data.dev_id) != 0) {
        perror("ublk managed swap activation"); goto out;
    }
    if (ublk_notify_ready() != 0) {
        perror("ublk readiness notification"); goto out;
    }
    printf("UBLK_LZO_READY /dev/ublkb%d\n", data.dev_id); fflush(stdout);
    for (;;) {
        pthread_mutex_lock(&workq.lock);
        int polling = workq.backend == QUEUE_BACKEND_CUDA;
        pthread_mutex_unlock(&workq.lock);
        if (polling) {
            poll_free_vram();
            const struct timespec timeout = { .tv_sec = 0, .tv_nsec = 250000000 };
            sig = sigtimedwait(&signals, NULL, &timeout);
            if (sig < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                perror("sigtimedwait"); goto out;
            }
        } else if (sigwait(&signals, &sig)) {
            fprintf(stderr, "sigwait failed\n"); goto out;
        }
        if (sig == SIGUSR2) {
            request_drain(NULL, 0, 0, 0);
            continue;
        }
        if (sig == SIGUSR1) { fprintf(stderr, "ublk queue exited unexpectedly\n"); goto out; }
        if (kernel_safe_stop || swap_table_empty()) break;
        fprintf(stderr, "UBLK_SHUTDOWN_DEFERRED swap active or state unavailable\n");
    }
    rc = 0;
out:
    /* Freeze kernel admission while the worker and completion owner can drain. */
    if (added && queue_started && queue_status > 0) {
        if (kernel_safe_stop && !start_attempted) {
            /* START was never submitted, so no block disk could have openers. */
            puts("UBLK_PRESTART_STOP_BEGIN"); fflush(stdout);
            if (ublksrv_ctrl_stop_dev(ctrl) < 0) stop_device_confirmed(ctrl, &signals);
            puts("UBLK_PRESTART_STOP_DONE"); fflush(stdout);
        } else stop_device_confirmed(ctrl, &signals);
    }
    if (worker_started) {
        pthread_mutex_lock(&workq.lock);
        workq.stopping = 1;
        pthread_cond_broadcast(&workq.cond);
        pthread_mutex_unlock(&workq.lock);
        pthread_join(workq.worker, NULL);
    }
    if (workq.spare_fd >= 0) {
        close(workq.spare_fd);
        workq.spare_fd = -1;
        workq.reserve_state = DISK_RESERVE_CLEANED;
    }
    if (worker_started) {
        pthread_mutex_lock(&workq.lock);
        for (;;) {
            int pending = 0;
            for (int i = 0; i < QDEPTH; i++) pending |= workq.jobs[i].state != JOB_FREE;
            if (!pending) break;
            if (workq.queue_exited) {
                fprintf(stderr, "ublk queue exited with pending I/O; cannot drain\n");
                _Exit(1);
            }
            pthread_cond_wait(&workq.cond, &workq.lock);
        }
        pthread_mutex_unlock(&workq.lock);
    }
    if (queue_started) pthread_join(qt, NULL);
    if (dev) ublksrv_dev_deinit(dev);
    if (added && ublksrv_ctrl_del_dev(ctrl) < 0) rc = 1;
    if (ctrl) ublksrv_ctrl_deinit(ctrl);
    if (safe_ring_ready) io_uring_queue_exit(&safe_ring);
    if (safe_ctrl_fd >= 0) close(safe_ctrl_fd);
    if (workq.backend == QUEUE_BACKEND_CUDA) {
        /* Never release DMA buffers after an unconfirmed bind or synchronization. */
        if (g_cu_ctx &&
            (_cuCtxSetCurrent(g_cu_ctx) != CUDA_SUCCESS ||
             _cuStreamSynchronize(NULL) != CUDA_SUCCESS ||
             (workq.stage && _cuMemFreeHost(workq.stage) != CUDA_SUCCESS) ||
             free_cuda_pool(&workq) != CUDA_SUCCESS ||
             _cuCtxDestroy(g_cu_ctx) != CUDA_SUCCESS)) {
            fprintf(stderr, "ublk CUDA cleanup failed; aborting resource teardown\n");
            _Exit(1);
        }
        if (g_libcuda) dlclose(g_libcuda);
    } else {
        free(workq.stage); free((void *)(uintptr_t)g_vram_ptr);
    }
    g_prepare_slot = NULL; g_retired_slot_freed = NULL; vmm_draining_queue = NULL;
    free(workq.vmm_refs); free(workq.vmm_released);
    free(workq.drain_page); compress_shutdown();
    return rc;
usage:
    fprintf(stderr, "usage: %s --device N --size BYTES --backing DIRECTORY [--host] [--kernel-safe-stop [--managed-swap]] [--cuda [--cuda-vmm] [--gpu-payload-bytes BYTES]]\nGPU capacity: default 2097152; range 65536..17179869184; multiple of 65536 (experimental). VMM physical blocks are allocated on demand with headroom checks.\n", argv[0]); return 2;
}
