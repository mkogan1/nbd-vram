#ifndef UBLK_SWAP_COMMAND_H
#define UBLK_SWAP_COMMAND_H

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int ublk_swap_command(char *const argv[], void (*tick)(void))
{
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    sigset_t empty;
    pid_t child;
    int error, status, wait_error_reported = 0;

    if (!argv || !argv[0] || argv[0][0] != '/') { errno = EINVAL; return -1; }

    sigemptyset(&empty);
    if ((error = posix_spawnattr_init(&attr))) goto spawn_error;
    if ((error = posix_spawnattr_setsigmask(&attr, &empty))) goto attr_error;
    if ((error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK))) goto attr_error;
    if ((error = posix_spawn_file_actions_init(&actions))) goto attr_error;
    if ((error = posix_spawn_file_actions_addclosefrom_np(&actions, 3))) goto actions_error;
    error = posix_spawn(&child, argv[0], &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (error) goto spawn_error;

    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && errno == ECHILD) { errno = EIO; return -1; }
        if (result < 0) {
            /* Unknown wait errors do not prove the child stopped; keep waiting. */
            if (!wait_error_reported) {
                fprintf(stderr, "UBLK_SWAP_COMMAND_WAIT_ERROR errno=%d resources_retained=1\n", errno);
                wait_error_reported = 1;
            }
        }
        if (tick) tick();
        {
            const struct timespec delay = { .tv_sec = 0, .tv_nsec = 250000000 };
            struct timespec remaining = delay;
            while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    errno = EIO;
    return -1;

actions_error:
    posix_spawn_file_actions_destroy(&actions);
attr_error:
    posix_spawnattr_destroy(&attr);
spawn_error:
    errno = error;
    return -1;
}

#endif
