#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static pid_t fixture_waitpid(pid_t, int *, int);
#define waitpid fixture_waitpid
#include "ublk-swap-command.h"
#undef waitpid

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d: %s)\n", __LINE__, #expr, errno, strerror(errno)); \
    exit(1); \
} } while (0)

static unsigned ticks;
static int inject_wait_errors, injected_wait_errors;
static pid_t fixture_waitpid(pid_t child, int *status, int flags)
{
    if (inject_wait_errors && injected_wait_errors < 2) {
        ++injected_wait_errors;
        errno = EINVAL;
        return -1;
    }
    return waitpid(child, status, flags);
}
static void count_tick(void) { ++ticks; }

static int run(char *const argv[], void (*tick)(void))
{
    errno = 0;
    return ublk_swap_command(argv, tick);
}

int main(void)
{
    char *yes[] = { "/bin/true", NULL };
    char *no[] = { "/bin/false", NULL };
    char *missing[] = { "/definitely/missing/ublk-swap-command", NULL };
    char *sleeping[] = { "/bin/sleep", "0.3", NULL };
    char *signal_child[] = { "/bin/sh", "-c", "kill -TERM $$", NULL };
    char *mask_child[] = { "/bin/sh", "-c", "grep -q '^SigBlk:.*0000000000000000' /proc/self/status", NULL };
    char *fd_child[] = { "/bin/sh", "-c", "test ! -e /proc/self/fd/9", NULL };
    sigset_t blocked, previous;

    CHECK(run(yes, NULL) == 0);
    CHECK(run(no, NULL) == -1 && errno == EIO);
    CHECK(run(missing, NULL) == -1 && errno == ENOENT);
    CHECK(run(signal_child, NULL) == -1 && errno == EIO);

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
    CHECK(run(mask_child, NULL) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0);

    {
        int fd = open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        if (fd != 9) {
            CHECK(dup2(fd, 9) == 9);
            close(fd);
        }
        CHECK(run(fd_child, NULL) == 0);
        close(9);
    }

    ticks = 0;
    CHECK(run(sleeping, count_tick) == 0);
    CHECK(ticks > 0);
    inject_wait_errors = 1;
    ticks = 0;
    CHECK(run(sleeping, count_tick) == 0);
    CHECK(injected_wait_errors == 2 && ticks >= 2);
    puts("PASS ublk_swap_command");
    return 0;
}
