/* Guest-only BLKROSET probe for the disposable /dev/ublkb0 harness. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ublk-guest-guard.h"

int main(int argc, char **argv)
{
    if (ublk_guest_guard()) {
        fprintf(stderr, "ublk guest guard rejected environment\n");
        return 1;
    }
    if (argc != 3 || (strcmp(argv[1], "--set") && strcmp(argv[1], "--clear")) ||
        strcmp(argv[2], "/dev/ublkb0")) {
        fprintf(stderr, "usage: %s --set|--clear /dev/ublkb0\n", argv[0]);
        return 2;
    }

    int fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror("open device");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode)) {
        fprintf(stderr, "device is not a block device\n");
        close(fd);
        return 1;
    }

    int requested = !strcmp(argv[1], "--set");
    int confirmed;
    if (ioctl(fd, BLKROSET, &requested) || ioctl(fd, BLKROGET, &confirmed)) {
        perror("read-only ioctl");
        close(fd);
        return 1;
    }
    if (confirmed != requested) {
        fprintf(stderr, "read-only state mismatch: requested=%d actual=%d\n",
                requested, confirmed);
        close(fd);
        return 1;
    }
    if (close(fd)) {
        perror("close device");
        return 1;
    }
    printf("UBLK_READONLY_CONFIRMED value=%d\n", confirmed);
    return 0;
}
