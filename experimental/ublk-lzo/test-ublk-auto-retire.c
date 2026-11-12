/* Mock-only inclusion of actual poll_free_vram/request_drain.
 * cc -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 *   -isystem /var/tmp/ublk-compare-src/include -isystem /var/tmp/ublk-compare-src \
 *   test-ublk-auto-retire.c -Wl,--gc-sections -pthread -ldl \
 *   -o /var/tmp/test-ublk-auto-retire
 * /var/tmp/test-ublk-auto-retire
 * No libublksrv, target main, worker, GPU, device, swap or mount.
 * shortcut: direct calls do not cover main's timed wait/poll scheduling;
 * qualify the event loop separately in an authorized environment.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
static void fixture_free(void *p);
static int fixture_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int fixture_open(const char *, int, ...);
static int fixture_ioctl(int, unsigned long, ...);
static ssize_t fixture_pread(int, void *, size_t, off_t);
static ssize_t fixture_pwrite(int, const void *, size_t, off_t);
static int fixture_fsync(int);
static int fixture_access(const char *, int);
#define free fixture_free
#define pthread_create fixture_create
#define open fixture_open
#define ioctl fixture_ioctl
#define pread fixture_pread
#define pwrite fixture_pwrite
#define fsync fixture_fsync
#define access fixture_access
int main(int, char **) __asm__("unused_ublk_target_main");
#include "ublk-lzo-target.c"
#undef free
#undef pthread_create
#undef open
#undef ioctl
#undef pread
#undef pwrite
#undef fsync
#undef access
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); _Exit(90); } } while (0)
static void fixture_free(void *p) { (void)p; CHECK(0); }
static int fixture_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p)
{ (void)t; (void)a; (void)f; (void)p; CHECK(0); return -1; }
static int fixture_open(const char *p, int f, ...) { (void)p; (void)f; CHECK(0); return -1; }
static int fixture_ioctl(int f, unsigned long r, ...) { (void)f; (void)r; CHECK(0); return -1; }
static ssize_t fixture_pread(int f, void *p, size_t n, off_t o)
{ (void)f; (void)p; (void)n; (void)o; CHECK(0); return -1; }
static ssize_t fixture_pwrite(int f, const void *p, size_t n, off_t o)
{ (void)f; (void)p; (void)n; (void)o; CHECK(0); return -1; }
static int fixture_fsync(int f) { (void)f; CHECK(0); return -1; }
static int marker_present, access_calls;
static int fixture_access(const char *p, int m)
{ CHECK(!strcmp(p, "/tmp/cuda-low-free") && m == F_OK); ++access_calls; return marker_present ? 0 : -1; }
static size_t available, total;
static CUresult result;
static unsigned queries;
static CUresult query(size_t *a, size_t *t)
{ ++queries; *a = available; *t = total; return result; }
static void untouched(void)
{
    struct job empty[QDEPTH] = {0};
    CHECK(!memcmp(workq.jobs, empty, sizeof(empty)));
    CHECK(!workq.started && !workq.queue_exited && !workq.stage && !workq.drain_page && !workq.ublk);
    CHECK(!g_pool_retired && !g_vram_ptr && !g_vram_size && !g_ptes && !g_slabs);
    CHECK(!g_nchunks && !g_vram_obj_bytes && g_back_fd == -1 && !g_libcuda);
    CHECK(!_cuMemFree && !_cuMemcpyHtoDAsync && !_cuMemcpyDtoHAsync);
}
static void reset(void)
{
    workq.drain_state = DRAIN_IDLE; workq.next_seq = 17; workq.drain_seq = 9;
    workq.drain_error = 0; workq.stopping = 0;
    untouched();
}
int auto_retire_test_main(void) __asm__("main");
int auto_retire_test_main(void)
{
    const size_t gib = (size_t)1024 * 1024 * 1024;
    const struct { const char *name; size_t total, available; CUresult result; int drain; } cases[] = {
        {"16GiB exact 2GiB boundary", 16*gib, 2*gib, 0, 0},
        {"16GiB below 2GiB", 16*gib, 2*gib-1, 0, 1},
        {"threshold capped at 2GiB", 32*gib, 2*gib, 0, 0},
        {"small total exact eighth", 1024, 128, 0, 0},
        {"small total below eighth", 1024, 127, 0, 1},
        {"query error", 16*gib, 16*gib, 1, 1},
        {"zero total", 0, 0, 0, 1},
        {"total below eight", 7, 7, 0, 1},
        {"free exceeds total", 1024, 1025, 0, 1},
    };
    _cuMemGetInfo = query;
    test_low_free = 0; marker_present = 0; access_calls = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        reset(); total = cases[i].total; available = cases[i].available; result = cases[i].result;
        unsigned before = queries;
        poll_free_vram();
        CHECK(queries == before + 1);
        CHECK(workq.drain_state == (cases[i].drain ? DRAIN_PENDING : DRAIN_IDLE));
        CHECK(workq.next_seq == (uint64_t)(17 + cases[i].drain));
        CHECK(workq.drain_seq == (uint64_t)(cases[i].drain ? 17 : 9));
        CHECK(!workq.drain_error); untouched();
        printf("PASS %s\n", cases[i].name);
    }
    reset(); _cuMemGetInfo = NULL; poll_free_vram();
    CHECK(workq.drain_state == DRAIN_PENDING && workq.drain_seq == 17 && workq.next_seq == 18);
    untouched();
    _cuMemGetInfo = query;
    reset(); total = 16*gib; available = 16*gib; result = 0;
    test_low_free = 0; marker_present = 1; access_calls = 0;
    poll_free_vram();
    CHECK(access_calls == 0 && workq.drain_state == DRAIN_IDLE); untouched();
    test_low_free = 1; marker_present = 0; access_calls = 0;
    poll_free_vram();
    CHECK(access_calls == 1 && workq.drain_state == DRAIN_IDLE); untouched();
    marker_present = 1; access_calls = 0;
    poll_free_vram();
    CHECK(access_calls == 1 && workq.drain_state == DRAIN_PENDING); untouched();
    reset(); total = 16*gib; available = 16*gib; result = 1; access_calls = 0;
    poll_free_vram();
    CHECK(access_calls == 0 && workq.drain_state == DRAIN_PENDING); untouched();
    test_low_free = 0; marker_present = 0;
    for (int state = DRAIN_PENDING; state <= DRAIN_FAILED; ++state) {
        reset(); workq.drain_state = state; workq.drain_error = EIO;
        for (unsigned i = 0; i < 3; ++i) {
            request_drain(NULL, 0, 0, 0); poll_free_vram();
            CHECK((int)workq.drain_state == state && workq.drain_seq == 9);
            CHECK(workq.next_seq == 17 && workq.drain_error == EIO); untouched();
        }
    }
    reset(); workq.next_seq = UINT64_MAX - 1; request_drain(NULL, 0, 0, 0);
    CHECK(workq.drain_state == DRAIN_PENDING && workq.drain_seq == UINT64_MAX - 1);
    CHECK(workq.next_seq == UINT64_MAX); request_drain(NULL, 0, 0, 0);
    CHECK(workq.drain_seq == UINT64_MAX - 1 && workq.next_seq == UINT64_MAX);
    reset(); workq.next_seq = UINT64_MAX;
    request_drain(NULL, 0, 0, 0); poll_free_vram();
    CHECK(workq.drain_state == DRAIN_IDLE && workq.next_seq == UINT64_MAX);
    CHECK(workq.drain_seq == 9 && !workq.drain_error); untouched();
    reset(); workq.stopping = 1; request_drain(NULL, 0, 0, 0);
    CHECK(workq.drain_state == DRAIN_IDLE && workq.next_seq == 17 && workq.drain_seq == 9);
    untouched();
    puts("MOCK_ONLY_PASS actual_poll=1 actual_request=1 worker=0 GPU=0 store=0 device=0");
    return 0;
}
