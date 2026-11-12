/* Guest-only held-read probe for an already-created disposable ublk device. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ublk-guest-guard.h"

enum { PAGE = 4096 };

static void pattern(unsigned char *buf)
{
    uint32_t x = 2001U | 1U;
    for (int i = 0; i < PAGE; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (unsigned char)x;
    }
}

int main(int argc, char **argv)
{
    if (ublk_guest_guard()) {
        fprintf(stderr, "ublk guest guard rejected environment\n");
        return 1;
    }
    if (argc != 3 || !*argv[2]) {
        fprintf(stderr, "usage: %s DISPOSABLE_BLOCK_DEVICE CAPACITY_BYTES\n", argv[0]);
        return 2;
    }
    errno = 0;
    char *end;
    uintmax_t capacity = strtoumax(argv[2], &end, 10);
    if (errno || *end || capacity < PAGE || capacity % PAGE) {
        fprintf(stderr, "capacity must be a positive multiple of 4096\n");
        return 2;
    }
    alarm(12);
    int fd = open(argv[1], O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0) { perror("open O_DIRECT"); return 1; }
    struct stat st;
    uint64_t actual = 0;
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || ioctl(fd, BLKGETSIZE64, &actual) ||
        actual != capacity) {
        fprintf(stderr, "not the expected block device capacity\n");
        close(fd); return 1;
    }
    unsigned char *buf = NULL, expected[PAGE];
    if (posix_memalign((void **)&buf, PAGE, PAGE)) {
        fprintf(stderr, "aligned allocation failed\n"); close(fd); return 1;
    }
    ssize_t n;
    do { n = pread(fd, buf, PAGE, 0); } while (n < 0 && errno == EINTR);
    pattern(expected);
    int rc = 1;
    if (n != PAGE) fprintf(stderr, "held read failed: %s\n", n < 0 ? strerror(errno) : "short read");
    else if (memcmp(buf, expected, PAGE)) fprintf(stderr, "seed-2001 page mismatch\n");
    else { puts("HELD_READ_PASS"); rc = 0; }
    free(buf);
    if (close(fd)) { perror("close"); rc = 1; }
    alarm(0);
    return rc;
}
