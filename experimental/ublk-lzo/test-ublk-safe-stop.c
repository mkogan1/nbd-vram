/* Mock-only tests of the safe stop control path; no real ring or device runs.
 * Build with the target's ublksrv headers, without linking libublksrv:
 * cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections \
 *   -fdata-sections -I/var/tmp/ublk-compare-src/include \
 *   -I/var/tmp/ublk-compare-src test-ublk-safe-stop.c \
 *   -Wl,--gc-sections -pthread -ldl -luring -o /var/tmp/test-ublk-safe-stop
 * /var/tmp/test-ublk-safe-stop
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <liburing.h>

struct ublksrv_ctrl_dev;
struct io_uring;
struct io_uring_sqe;
struct io_uring_cqe;
static struct io_uring_sqe *mock_ring_get_sqe(struct io_uring *ring);
static int mock_ring_submit(struct io_uring *ring);
static int mock_ring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe);
static void mock_ring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe);
static int fixture_sigwait(const sigset_t *set, int *sig);
static int mock_stop_dev(struct ublksrv_ctrl_dev *ctrl);

int main(int, char **) __asm__("unused_ublk_target_main");
#define io_uring_get_sqe mock_ring_get_sqe
#define io_uring_submit mock_ring_submit
#define io_uring_wait_cqe mock_ring_wait_cqe
#define io_uring_cqe_seen mock_ring_cqe_seen
#define sigwait fixture_sigwait
#define ublksrv_ctrl_stop_dev mock_stop_dev
#include "ublk-lzo-target.c"
#undef io_uring_get_sqe
#undef io_uring_submit
#undef io_uring_wait_cqe
#undef io_uring_cqe_seen
#undef sigwait
#undef ublksrv_ctrl_stop_dev
#undef io_uring_get_sqe
#undef io_uring_submit
#undef io_uring_wait_cqe
#undef io_uring_cqe_seen

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s line %d: %s\n", case_name, __LINE__, #expr); \
    _Exit(90); \
} } while (0)

enum scenario { RETRY_BUSY, RETRY_UNSUPPORTED, WAIT_INTERRUPTED };
static const char *case_name;
static union { struct io_uring_sqe sqe; unsigned char storage[128]; } test_entry;
static struct io_uring_cqe test_cqe;
static int stop_calls;
static int submit_result, submit_calls, get_sqe_calls, wait_errors[4], wait_results[4];
static int wait_count, seen_calls, signal_pos, signal_count, signals[4], swap_calls;

static struct io_uring_sqe *mock_ring_get_sqe(struct io_uring *ring)
{
    CHECK(ring == &safe_ring);
    ++get_sqe_calls;
    memset(&test_entry, 0, sizeof(test_entry));
    return &test_entry.sqe;
}

static int mock_ring_submit(struct io_uring *ring)
{
    CHECK(ring == &safe_ring);
    ++submit_calls;
    if (submit_result != 1) return submit_result;
    CHECK(test_entry.sqe.opcode == IORING_OP_URING_CMD);
    CHECK(test_entry.sqe.fd == safe_ctrl_fd);
    CHECK(test_entry.sqe.cmd_op == UBLK_U_CMD_TRY_STOP_DEV);
    struct ublksrv_ctrl_cmd cmd;
    memcpy(&cmd, test_entry.sqe.cmd, sizeof(cmd));
    CHECK(cmd.dev_id == (__u32)safe_device_id && cmd.queue_id == UINT16_MAX);
    return 1;
}

static int mock_ring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe)
{
    CHECK(ring == &safe_ring && cqe);
    CHECK(wait_count < (int)(sizeof(wait_errors) / sizeof(wait_errors[0])));
    int i = wait_count++;
    if (wait_errors[i]) return wait_errors[i];
    test_cqe.res = wait_results[i];
    *cqe = &test_cqe;
    return 0;
}

static void mock_ring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe)
{
    CHECK(ring == &safe_ring && cqe == &test_cqe);
    ++seen_calls;
}

static int fixture_sigwait(const sigset_t *set, int *sig)
{
    CHECK(set && sig && signal_pos < signal_count);
    *sig = signals[signal_pos++];
    CHECK(sigismember(set, *sig) == 1);
    return 0;
}

static int mock_stop_dev(struct ublksrv_ctrl_dev *ctrl)
{
    (void)ctrl;
    ++stop_calls;
    return -EIO;
}

static void child_test(enum scenario which, const char *name)
{
    sigset_t set;
    case_name = name;
    kernel_safe_stop = safe_ring_ready = 1;
    safe_control_broken = 0;
    safe_ctrl_fd = 42;
    safe_device_id = 17;
    stop_calls = submit_calls = get_sqe_calls = wait_count = seen_calls = 0;
    signal_pos = swap_calls = 0;
    submit_result = 1;
    memset(wait_errors, 0, sizeof(wait_errors));
    memset(wait_results, 0, sizeof(wait_results));
    switch (which) {
    case RETRY_BUSY:
        wait_results[0] = -EBUSY; wait_results[1] = 0;
        signals[0] = SIGTERM; signal_count = 1;
        break;
    case RETRY_UNSUPPORTED:
        wait_results[0] = -EOPNOTSUPP; wait_results[1] = 0;
        signals[0] = SIGINT; signal_count = 1;
        break;
    case WAIT_INTERRUPTED:
        wait_errors[0] = -EINTR; wait_results[1] = 0;
        signal_count = 0;
        break;
    }
    sigemptyset(&set);
    sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
    int result = stop_device_confirmed((struct ublksrv_ctrl_dev *)(uintptr_t)1, &set);
    CHECK(result == 0);
    CHECK(stop_calls == 0 && swap_calls == 0);
    if (which == RETRY_BUSY || which == RETRY_UNSUPPORTED) {
        CHECK(submit_calls == 2 && get_sqe_calls == 2 && wait_count == 2 && seen_calls == 2);
    } else {
        CHECK(submit_calls == 1 && get_sqe_calls == 1 && wait_count == 2 && seen_calls == 1);
    }
    CHECK(signal_pos == signal_count);
    _Exit(0);
}

int safe_stop_test_main(void) __asm__("main");
int safe_stop_test_main(void)
{
    for (enum scenario which = RETRY_BUSY; which <= WAIT_INTERRUPTED; ++which) {
        const char *name = which == RETRY_BUSY ? "busy-retained-term-retry" :
                           which == RETRY_UNSUPPORTED ? "unsupported-retained-int-retry" :
                           "wait-eintr-retried";
        pid_t child = fork();
        int status;
        CHECK(child >= 0);
        if (!child) child_test(which, name);
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        CHECK(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        printf("PASS %s\n", name);
    }
    case_name = "submit-failure-latches-broken";
    kernel_safe_stop = safe_ring_ready = 1;
    safe_control_broken = 0;
    safe_ctrl_fd = 42;
    safe_device_id = 17;
    submit_result = -EINTR;
    submit_calls = get_sqe_calls = wait_count = seen_calls = 0;
    CHECK(kernel_try_stop() == -EIO);
    CHECK(safe_control_broken && submit_calls == 1 && get_sqe_calls == 1);
    CHECK(wait_count == 0 && seen_calls == 0);
    CHECK(kernel_try_stop() == -EIO && submit_calls == 1 && get_sqe_calls == 1);
    puts("PASS submit-failure-latches-broken");
    puts("MOCK_ONLY_PASS safe_helper=1 real_ring=0 real_device=0");
    return 0;
}
