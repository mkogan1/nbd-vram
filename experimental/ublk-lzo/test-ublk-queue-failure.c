/* Safe mock-only queue fault injection; no VM, device, backing or CUDA needed.
 * Build (GNU C, real ublksrv headers only; DO NOT link libublksrv):
 * cc -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections \
 *   -I/var/tmp/ublk-compare-src/include -I/var/tmp/ublk-compare-src \
 *   test-ublk-queue-failure.c -Wl,--gc-sections -pthread -ldl \
 *   -o /var/tmp/test-ublk-queue-failure
 * /var/tmp/test-ublk-queue-failure
 *
 * Covers the unchanged target queue_thread, including its actual self-signal.
 * shortcut: main's sigwait/failure policy is an isolated seam below, run a
 * VM integration test to qualify startup, worker/pending-I/O drain and teardown.
 * No target main/worker is called; unused target code must be linker-discarded.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

/* The target undefines main internally; an asm label renames its entry symbol
 * without editing it or also renaming the embedded NBD entry point. */
int main(int, char **) __asm__("unused_ublk_target_main");
static int fixture_kill(pid_t pid, int sig);
#define kill fixture_kill
#define ublksrv_queue_init fixture_queue_init
#define ublksrv_process_io fixture_process_io
#define ublksrv_queue_deinit fixture_queue_deinit
#include "ublk-lzo-target.c"
#undef kill
#undef ublksrv_queue_init
#undef ublksrv_process_io
#undef ublksrv_queue_deinit

#include <sys/wait.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); _Exit(90); \
} } while (0)

enum scenario { EARLY_FAILURE, CONCURRENT_FAILURE, INIT_FAILURE, LOST_WAKE };
static enum scenario scenario;
static struct ublksrv_queue fake_queue;
static struct ublksrv_dev fake_dev;
static int process_calls, deinit_calls, signal_calls;
static int waiter_ready;

const struct ublksrv_queue *fixture_queue_init(const struct ublksrv_dev *dev,
                                              unsigned short id, void *data)
{
    CHECK(dev == &fake_dev && id == 0 && data == NULL);
    return scenario == INIT_FAILURE ? NULL : &fake_queue;
}

int fixture_process_io(const struct ublksrv_queue *q)
{
    CHECK(q == &fake_queue && pthread_equal(workq.owner, pthread_self()));
    if (scenario == CONCURRENT_FAILURE) {
        pthread_mutex_lock(&init_lock);
        while (!waiter_ready) pthread_cond_wait(&init_cond, &init_lock);
        pthread_mutex_unlock(&init_lock);
        /* Both zero and positive results must keep the real loop running. */
        if (++process_calls <= 3) return process_calls == 1 ? 0 : 1;
        return -EIO;
    }
    ++process_calls;
    return -ENODEV;
}

void fixture_queue_deinit(const struct ublksrv_queue *q)
{
    CHECK(q == &fake_queue && workq.ublk == NULL && workq.queue_exited);
    ++deinit_calls;
}

static int fixture_kill(pid_t pid, int sig)
{
    CHECK(pid == getpid() && sig == SIGUSR1);
    CHECK(deinit_calls == 1 && workq.queue_exited && workq.ublk == NULL);
    ++signal_calls;
    return scenario == LOST_WAKE ? 0 : kill(getpid(), SIGUSR1);
}

static void expected_timeout(int sig)
{
    (void)sig;
    _Exit(91);
}

static void child_test(enum scenario which)
{
    sigset_t signals;
    pthread_t thread;
    int sig = 0, rc = 1;
    scenario = which;
    /* A missing wake or deadlocked join fails boundedly, even with NDEBUG. */
    CHECK(signal(SIGALRM, SIG_DFL) != SIG_ERR);
    sigemptyset(&signals);
    sigaddset(&signals, SIGALRM);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &signals, NULL) == 0);
    alarm(2);
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &signals, NULL) == 0);
    CHECK(pthread_create(&thread, NULL, queue_thread, &fake_dev) == 0);
    pthread_mutex_lock(&init_lock);
    while (!queue_status) pthread_cond_wait(&init_cond, &init_lock);
    CHECK(queue_status == (which == INIT_FAILURE ? -1 : 1));
    waiter_ready = 1;
    pthread_cond_broadcast(&init_cond);
    pthread_mutex_unlock(&init_lock);

    if (which == INIT_FAILURE) {
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(!workq.queue_exited && !workq.ublk);
        CHECK(!process_calls && !deinit_calls && !signal_calls);
        CHECK(sigpending(&signals) == 0 && !sigismember(&signals, SIGUSR1));
        _Exit(rc);
    }
    if (which != CONCURRENT_FAILURE) {
        /* Deterministic pre-wait exit: SIGUSR1 must remain pending. */
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(sigpending(&signals) == 0);
        CHECK(sigismember(&signals, SIGUSR1) == (which != LOST_WAKE));
        sigemptyset(&signals);
        sigaddset(&signals, SIGUSR1);
    }

    /* Install only after setup/join checks: an earlier hang must not pass. */
    if (which == LOST_WAKE) {
        CHECK(signal(SIGALRM, expected_timeout) != SIG_ERR);
        alarm(2);
    }
    /* Isolated equivalent of main's SIGUSR1 branch, NOT full target main. */
    CHECK(sigwait(&signals, &sig) == 0);
    if (sig != SIGUSR1) rc = 0;
    CHECK(sig == SIGUSR1 && rc == 1);
    if (which == CONCURRENT_FAILURE) CHECK(pthread_join(thread, NULL) == 0);
    CHECK(workq.queue_exited && !workq.ublk);
    CHECK(deinit_calls == 1 && signal_calls == 1);
    CHECK(process_calls == (which == CONCURRENT_FAILURE ? 4 : 1));
    CHECK(sigpending(&signals) == 0 && !sigismember(&signals, SIGUSR1));
    _Exit(rc);
}

int queue_failure_test_main(void) __asm__("main");
int queue_failure_test_main(void)
{
    static const char *names[] = {
        "exit-before-sigwait", "concurrent-exit-after-nonnegative-results",
        "queue-init-failure", "suppressed-wake-watchdog-control"
    };
    for (enum scenario which = EARLY_FAILURE; which <= LOST_WAKE; ++which) {
        pid_t child = fork();
        int status;
        CHECK(child >= 0);
        if (!child) child_test(which);
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        CHECK(result == child);
        if (which == LOST_WAKE)
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 91);
        else
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
        printf("PASS %s\n", names[which]);
    }
    puts("MOCK_ONLY_PASS real_queue_thread=1 real_sigwait=1 full_target_main=0");
    return 0;
}
