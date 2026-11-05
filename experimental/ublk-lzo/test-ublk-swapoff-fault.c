#define _GNU_SOURCE
#include "ublk-guest-guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    static char *const swapoff_argv[] = {
        "/usr/bin/swapoff.real", "--", "/dev/ublkb0", NULL
    };
    struct stat st;
    int fd;

    if (ublk_guest_guard() != 0)
        return 1;
    if (argc != 3 || strcmp(argv[1], "--") || strcmp(argv[2], "/dev/ublkb0"))
        return 1;

    fd = open("/tmp/managed-swapoff-failed",
              O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (close(fd) != 0)
            return 1;
        fprintf(stderr, "UBLK_SWAPOFF_FAULT synthetic=1 call=1\n");
        return 1;
    }
    if (errno != EEXIST)
        return 1;
    if (lstat("/tmp/managed-swapoff-failed", &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        st.st_size != 0)
        return 1;

    execv(swapoff_argv[0], swapoff_argv);
    return 1;
}
