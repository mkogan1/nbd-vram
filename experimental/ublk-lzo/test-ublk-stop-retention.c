/* Mock-only tests of stop_device_confirmed; no target main or real device runs.
 * Build with the target's ublksrv headers, without linking libublksrv:
 * cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections \
 *   -fdata-sections -isystem /var/tmp/ublk-compare-src/include \
 *   -isystem /var/tmp/ublk-compare-src test-ublk-stop-retention.c \
 *   -Wl,--gc-sections -pthread -ldl -o /var/tmp/test-ublk-stop-retention
 * /var/tmp/test-ublk-stop-retention
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
#include <time.h>

static FILE *fixture_fopen(const char *path, const char *mode);
static int fixture_fclose(FILE *stream);
static int fixture_sigwait(const sigset_t *set, int *sig);
static int fixture_nanosleep(const struct timespec *duration, struct timespec *remaining);
struct ublksrv_ctrl_dev;
static int fixture_stop_dev(struct ublksrv_ctrl_dev *ctrl);

int main(int, char **) __asm__("unused_ublk_target_main");
#define fopen fixture_fopen
#define fclose fixture_fclose
#define sigwait fixture_sigwait
#define nanosleep fixture_nanosleep
#define ublksrv_ctrl_stop_dev fixture_stop_dev
#include "ublk-lzo-target.c"
#undef fopen
#undef fclose
#undef sigwait
#undef nanosleep
#undef ublksrv_ctrl_stop_dev

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s line %d: %s\n", case_name, __LINE__, #expr); \
    _Exit(90); \
} } while (0)

enum scenario { FAIL_THEN_TERM, IGNORE_USR1, INITIAL_ACTIVE_SWAP,
                REPEATED_FAILURE, STOP_SUCCEEDS_FIRST, WAIT_ERROR_RETAINS };
static const char *case_name;
static int stop_results[4], stop_count, stop_calls;
static int signals[8], signal_errors[8], signal_count, signal_pos, wait_calls;
static int swap_empty, become_empty_after_check, swap_checks;
static int sleep_calls;
static FILE *opened;

static FILE *fixture_fopen(const char *path, const char *mode)
{
    static const char empty[] = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n";
    static const char active[] = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/dev/swap\tfile\t1024\t1\t-2\n";
    const char *contents = (swap_empty || (become_empty_after_check && swap_checks)) ? empty : active;
    CHECK(!strcmp(path, "/proc/swaps") && !strcmp(mode, "r"));
    ++swap_checks;
    opened = fmemopen((void *)contents, strlen(contents), "r");
    CHECK(opened != NULL);
    return opened;
}

static int fixture_fclose(FILE *stream)
{
    CHECK(stream == opened);
    opened = NULL;
    return fclose(stream);
}

static int fixture_stop_dev(struct ublksrv_ctrl_dev *ctrl)
{
    CHECK(ctrl == (struct ublksrv_ctrl_dev *)(uintptr_t)1);
    CHECK(stop_calls < stop_count);
    return stop_results[stop_calls++];
}

static int fixture_sigwait(const sigset_t *set, int *sig)
{
    CHECK(set && sig && signal_pos < signal_count);
    ++wait_calls;
    if (signal_errors[signal_pos]) {
        ++signal_pos;
        return EINVAL;
    }
    *sig = signals[signal_pos++];
    CHECK(sigismember(set, *sig) == 1);
    return 0;
}

static int fixture_nanosleep(const struct timespec *duration, struct timespec *remaining)
{
    CHECK(duration && duration->tv_sec == 0 && duration->tv_nsec == 250000000);
    CHECK(remaining == NULL);
    ++sleep_calls;
    return 0;
}

static void expected_timeout(int sig)
{
    (void)sig;
    _Exit(91);
}

static void child_test(enum scenario which, const char *name)
{
    sigset_t set;
    struct ublksrv_ctrl_dev *ctrl = (struct ublksrv_ctrl_dev *)(uintptr_t)1;
    int result;
    case_name = name;
    CHECK(signal(SIGALRM, expected_timeout) != SIG_ERR);
    alarm(3);
    swap_empty = 1;
    become_empty_after_check = 0;
    switch (which) {
    case FAIL_THEN_TERM:
        stop_results[0] = -EIO; stop_results[1] = 0; stop_count = 2;
        swap_empty = 1;
        signals[0] = SIGTERM; signal_count = 1;
        break;
    case IGNORE_USR1:
        stop_results[0] = -EIO; stop_results[1] = 0; stop_count = 2;
        swap_empty = 1;
        signals[0] = SIGUSR1; signals[1] = SIGINT; signal_count = 2;
        break;
    case INITIAL_ACTIVE_SWAP:
        stop_results[0] = 0; stop_count = 1;
        swap_empty = 0; become_empty_after_check = 1;
        signals[0] = SIGTERM; signal_count = 1;
        break;
    case REPEATED_FAILURE:
        stop_results[0] = -EIO; stop_results[1] = -EBUSY;
        stop_results[2] = 0; stop_count = 3;
        swap_empty = 1;
        signals[0] = SIGTERM; signals[1] = SIGUSR2;
        signals[2] = SIGINT; signal_count = 3;
        break;
    case STOP_SUCCEEDS_FIRST:
        stop_results[0] = 0; stop_count = 1;
        break;
    case WAIT_ERROR_RETAINS:
        stop_results[0] = -EIO; stop_results[1] = 0; stop_count = 2;
        swap_empty = 1;
        signal_errors[0] = 1; signals[1] = SIGINT; signal_count = 2;
        break;
    }
    sigemptyset(&set);
    sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1); sigaddset(&set, SIGUSR2);
    result = stop_device_confirmed(ctrl, &set);
    CHECK(result == 0 && stop_calls == stop_count);
    CHECK(wait_calls == (which == STOP_SUCCEEDS_FIRST ? 0 : signal_count));
    CHECK(sleep_calls == (which == WAIT_ERROR_RETAINS ? 1 : 0));
    CHECK((workq.drain_state == DRAIN_PENDING) == (which == REPEATED_FAILURE));
    CHECK((workq.next_seq != 0) == (which == REPEATED_FAILURE));
    CHECK(swap_checks == (which == INITIAL_ACTIVE_SWAP || which == FAIL_THEN_TERM ||
                          which == IGNORE_USR1 || which == WAIT_ERROR_RETAINS ? 2 :
                          which == STOP_SUCCEEDS_FIRST ? 1 : 3));
    CHECK(opened == NULL);
    _Exit(0);
}

int stop_retention_test_main(void) __asm__("main");
int stop_retention_test_main(void)
{
    static const char *names[] = { "failure-then-term", "usr1-does-not-retry",
        "initial-active-swap-defers-first-stop", "repeated-stop-failure", "success-without-wait",
        "sigwait-error-retains" };
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (enum scenario which = FAIL_THEN_TERM; which <= WAIT_ERROR_RETAINS; ++which) {
        pid_t child = fork();
        int status;
        CHECK(child >= 0);
        if (!child) child_test(which, names[which]);
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        CHECK(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        printf("PASS %s\n", names[which]);
    }
    puts("MOCK_ONLY_PASS real_stop_helper=1 real_swap_parser=1 full_target_main=0");
    return 0;
}
