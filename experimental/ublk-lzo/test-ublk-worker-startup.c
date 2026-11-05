/* Safe mock-only inclusion test of the actual worker startup.
 * cc -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 *   -isystem /var/tmp/ublk-compare-src/include -isystem /var/tmp/ublk-compare-src \
 *   test-ublk-worker-startup.c -Wl,--gc-sections -pthread -ldl \
 *   -o /var/tmp/test-ublk-worker-startup
 * /var/tmp/test-ublk-worker-startup
 * No libublksrv linkage, target main, devices, GPU, backing, swap or mounts.
 */
#define _GNU_SOURCE
#include <sys/prctl.h>
static int fixture_prctl(int, ...);
int main(int, char **) __asm__("unused_ublk_target_main");
#define prctl fixture_prctl
#include "ublk-lzo-target.c"
#undef prctl

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); _Exit(90); \
} } while (0)

enum scenario { FLUSHER_FAILURE, CONTEXT_FAILURE, MOCK_SUCCESS };
static enum scenario scenario;
static unsigned prctl_calls, context_calls;
static char tls_stage, tls_lzo;

static int fixture_prctl(int option, ...)
{
    CHECK(option == PR_SET_IO_FLUSHER && ++prctl_calls == 1);
    CHECK(!context_calls);
    if (scenario == FLUSHER_FAILURE) { errno = EPERM; return -1; }
    return 0;
}

static CUresult fixture_context(CUcontext ctx)
{
    CHECK(scenario == CONTEXT_FAILURE && prctl_calls == 1);
    CHECK(ctx == g_cu_ctx && ++context_calls == 1);
    return 1;
}

static CUresult reject_upload(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)dst; (void)src; (void)n; (void)stream;
    CHECK(0); return 1;
}

static CUresult reject_download(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)dst; (void)src; (void)n; (void)stream;
    CHECK(0); return 1;
}

static CUresult reject_sync(CUstream stream)
{
    (void)stream; CHECK(0); return 1;
}

static CUresult reject_free(CUdeviceptr ptr)
{
    (void)ptr; CHECK(0); return 1;
}

int ublksrv_queue_send_event(const struct ublksrv_queue *q)
{
    (void)q; CHECK(0); return -1;
}

static void *run_worker(void *opaque)
{
    t_cstage = &tls_stage;
    t_lzo_work = &tls_lzo;
    CHECK(worker(opaque) == NULL);
    if (scenario == MOCK_SUCCESS) {
        CHECK(t_cstage == NULL && t_lzo_work == NULL);
    } else {
        /* Startup errors must return before installing or clearing stage TLS. */
        CHECK(t_cstage == &tls_stage && t_lzo_work == &tls_lzo);
    }
    return NULL;
}

int worker_startup_test_main(void) __asm__("main");
int worker_startup_test_main(void)
{
    sigset_t alarms;
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    void *stage = mmap(NULL, (size_t)page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(stage != MAP_FAILED);
    CHECK(signal(SIGALRM, SIG_DFL) != SIG_ERR);
    CHECK(sigemptyset(&alarms) == 0 && sigaddset(&alarms, SIGALRM) == 0);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &alarms, NULL) == 0);
    _cuCtxSetCurrent = fixture_context;
    _cuMemcpyHtoDAsync = reject_upload;
    _cuMemcpyDtoHAsync = reject_download;
    _cuStreamSynchronize = reject_sync;
    _cuMemFree = reject_free;
    for (scenario = FLUSHER_FAILURE; scenario <= MOCK_SUCCESS; ++scenario) {
        struct queue q = {
            .lock = PTHREAD_MUTEX_INITIALIZER,
            .cond = PTHREAD_COND_INITIALIZER,
            .backend = scenario == MOCK_SUCCESS ? QUEUE_BACKEND_MOCK : QUEUE_BACKEND_CUDA,
            .stopping = scenario == MOCK_SUCCESS,
            .stage = stage,
        };
        int expected = scenario == FLUSHER_FAILURE ? -EPERM :
                       scenario == CONTEXT_FAILURE ? -EIO : 1;
        prctl_calls = context_calls = 0;
        if (scenario != MOCK_SUCCESS) {
            /* Both store work and a drain must remain untouched on failure. */
            q.jobs[0].state = JOB_QUEUED;
            q.jobs[0].op = UBLK_IO_OP_WRITE;
            q.jobs[0].result = 123;
            q.drain_state = DRAIN_PENDING;
            q.drain_seq = 1;
        }
        alarm(3);
        CHECK(pthread_mutex_lock(&q.lock) == 0);
        CHECK(pthread_create(&q.worker, NULL, run_worker, &q) == 0);
        while (!q.started) CHECK(pthread_cond_wait(&q.cond, &q.lock) == 0);
        CHECK(q.started == expected);
        CHECK(pthread_mutex_unlock(&q.lock) == 0);
        CHECK(pthread_join(q.worker, NULL) == 0);
        alarm(0);
        CHECK(prctl_calls == 1 && context_calls == (unsigned)(scenario == CONTEXT_FAILURE));
        CHECK(!g_pool_retired && g_back_fd == -1 && !g_ptes && !g_vram_ptr);
        if (scenario != MOCK_SUCCESS) {
            CHECK(q.jobs[0].state == JOB_QUEUED && q.jobs[0].result == 123);
            CHECK(q.drain_state == DRAIN_PENDING && !q.drain_error);
        }
        CHECK(pthread_cond_destroy(&q.cond) == 0);
        CHECK(pthread_mutex_destroy(&q.lock) == 0);
        printf("PASS %s\n", scenario == FLUSHER_FAILURE ? "io-flusher-failure" :
               scenario == CONTEXT_FAILURE ? "cuda-context-failure" : "mock-success-stop");
    }
    CHECK(munmap(stage, (size_t)page_size) == 0);
    return 0;
}
