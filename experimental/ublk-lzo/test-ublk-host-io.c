#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "ublksrv.h"

enum { PAGE_SIZE_BYTES = 4096 };

static int parse_decimal(const char *s, uintmax_t max, uintmax_t *value)
{
    if (!*s) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    uintmax_t n = strtoumax(s, &end, 10);
    if (errno || *end || n > max) return -1;
    *value = n;
    return 0;
}

static int io_page(int fd, void *buf, off_t offset, int write_op)
{
    ssize_t n;
    do {
        n = write_op ? pwrite(fd, buf, PAGE_SIZE_BYTES, offset)
                     : pread(fd, buf, PAGE_SIZE_BYTES, offset);
    } while (n < 0 && errno == EINTR);
    if (n != PAGE_SIZE_BYTES) {
        fprintf(stderr, "%s offset=%jd: %s (returned %zd)\n",
                write_op ? "write" : "read", (intmax_t)offset,
                n < 0 ? strerror(errno) : "short I/O", n);
        return -1;
    }
    return 0;
}

static void make_page(unsigned char *buf, unsigned page)
{
    if (page % 4 == 0) {
        for (unsigned i = 0; i < PAGE_SIZE_BYTES; ++i)
            buf[i] = (unsigned char)("LZO-pattern-0123456789abcdef"[i % 29]);
        return;
    }
    uint32_t x = UINT32_C(0x9e3779b9) ^ (page * UINT32_C(0x85ebca6b));
    if (!x) x = 1;
    for (unsigned i = 0; i < PAGE_SIZE_BYTES; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (unsigned char)x;
    }
}

static int valid_info(struct ublksrv_ctrl_dev *ctrl, int id, pid_t expected_pid)
{
    const struct ublksrv_ctrl_dev_info *info;
    if (ublksrv_ctrl_get_info(ctrl) < 0) {
        fprintf(stderr, "fresh ublk get_info failed\n");
        return -1;
    }
    info = ublksrv_ctrl_get_dev_info(ctrl);
    if (!info || info->dev_id != (uint32_t)id || info->state != UBLK_S_DEV_LIVE ||
        info->ublksrv_pid != expected_pid) {
        fprintf(stderr, "ublk identity/state mismatch (id=%u state=%u pid=%d)\n",
                info ? info->dev_id : UINT32_MAX,
                info ? info->state : UINT16_MAX,
                info ? info->ublksrv_pid : -1);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uintmax_t id_value, pid_value, capacity_arg;
    int verify_only;
    struct ublksrv_dev_data data = { .dev_id = -1 };
    struct ublksrv_ctrl_dev *ctrl = NULL;
    unsigned char *expected = NULL, *actual = NULL;
    int fd = -1, rc = 1;
    char path[64], sys_path[128], resolved[4096];
    struct stat st;
    uint64_t actual_capacity = 0;
    uintmax_t page_count;

    verify_only = argc == 5 && !strcmp(argv[4], "--verify-only");
    if ((argc != 4 && !verify_only) || parse_decimal(argv[1], 4095, &id_value) ||
        parse_decimal(argv[2], INT_MAX, &pid_value) || pid_value == 0 ||
        parse_decimal(argv[3], UINT64_MAX, &capacity_arg) || capacity_arg < UINT64_C(4194304) ||
        capacity_arg > UINT64_C(34359738368) || capacity_arg % PAGE_SIZE_BYTES) {
        fprintf(stderr, "usage: %s id expectedPID capacity_bytes [--verify-only]\n", argv[0]);
        return 2;
    }
    int id = (int)id_value;
    pid_t expected_pid = (pid_t)pid_value;
    if ((uintmax_t)expected_pid != pid_value) {
        fprintf(stderr, "expectedPID out of range\n");
        return 2;
    }
    uint64_t capacity = (uint64_t)capacity_arg;
    page_count = capacity / PAGE_SIZE_BYTES;

    data.dev_id = id;
    ctrl = ublksrv_ctrl_init(&data);
    if (!ctrl) {
        fprintf(stderr, "ublksrv_ctrl_init failed\n");
        goto done;
    }
    if (valid_info(ctrl, id, expected_pid)) goto done;

    snprintf(path, sizeof(path), "/dev/ublkb%d", id);
    fd = open(path, (verify_only ? O_RDONLY : O_RDWR) | O_EXCL | O_DIRECT |
                   O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror("open private ublk device");
        goto done;
    }
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) ||
        ioctl(fd, BLKGETSIZE64, &actual_capacity) < 0 || actual_capacity != capacity) {
        fprintf(stderr, "block device type or requested capacity check failed\n");
        goto done;
    }
    snprintf(sys_path, sizeof(sys_path), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(sys_path, resolved)) {
        perror("realpath sysfs block device");
        goto done;
    }
    const char *base = strrchr(resolved, '/');
    snprintf(path, sizeof(path), "ublkb%d", id);
    if (!base || strcmp(base + 1, path)) {
        fprintf(stderr, "sysfs block-device name mismatch\n");
        goto done;
    }
    if (valid_info(ctrl, id, expected_pid)) goto done;

    if (posix_memalign((void **)&expected, PAGE_SIZE_BYTES, PAGE_SIZE_BYTES) ||
        posix_memalign((void **)&actual, PAGE_SIZE_BYTES, PAGE_SIZE_BYTES)) {
        fprintf(stderr, "aligned page allocation failed\n");
        goto done;
    }
    if (!verify_only) {
        for (unsigned page = 0; page < page_count; ++page) {
            make_page(expected, page);
            if (io_page(fd, expected, (off_t)page * PAGE_SIZE_BYTES, 1)) goto done;
        }
    }
    for (unsigned page = 0; page < page_count; ++page) {
        make_page(expected, page);
        if (io_page(fd, actual, (off_t)page * PAGE_SIZE_BYTES, 0)) goto done;
        if (memcmp(expected, actual, PAGE_SIZE_BYTES)) {
            fprintf(stderr, "page verification mismatch at page=%u\n", page);
            goto done;
        }
        if (page == 0) {
            puts("HOST_UBLK_FIRST_PAGE_VERIFIED");
            fflush(stdout);
        }
    }
    printf("HOST_UBLK_IO_PASS bytes=%" PRIu64 " verify_only=%d\n", capacity, verify_only);
    rc = 0;

done:
    free(expected);
    free(actual);
    if (fd >= 0 && close(fd)) { perror("close block device"); rc = 1; }
    if (ctrl) ublksrv_ctrl_deinit(ctrl);
    return rc;
}
