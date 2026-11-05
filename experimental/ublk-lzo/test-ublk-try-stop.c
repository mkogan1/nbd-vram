#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ublk_cmd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <liburing.h>

#include "ublk-guest-guard.h"

#ifndef UBLK_U_CMD_TRY_STOP_DEV
#define UBLK_U_CMD_TRY_STOP_DEV _IOWR('u', 0x17, struct ublksrv_ctrl_cmd)
#endif

int main(int argc, char **argv)
{
    struct io_uring ring;
    struct io_uring_params params = { .flags = IORING_SETUP_SQE128 };
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    struct ublksrv_ctrl_cmd cmd = { .dev_id = 0, .queue_id = UINT16_MAX };
    struct __kernel_timespec timeout = { .tv_sec = 5 };
    const char *expected;
    int busy, fd, ret, status = 1;

    if (argc != 2 || (strcmp(argv[1], "--busy") && strcmp(argv[1], "--stop"))) {
        fprintf(stderr, "usage: %s --busy|--stop\n", argv[0]);
        return 2;
    }
    if (ublk_guest_guard() != 0) {
        fprintf(stderr, "UBLK_TRY_STOP_PROBE refused: VM guard failed\n");
        return 1;
    }
    busy = !strcmp(argv[1], "--busy");
    expected = busy ? "busy" : "stopped";

    fd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/ublk-control");
        return 1;
    }
    ret = io_uring_queue_init_params(2, &ring, &params);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init_params: %s\n", strerror(-ret));
        goto out_close;
    }
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "io_uring_get_sqe returned NULL\n");
        goto out_ring;
    }
    memset(sqe, 0, sizeof(*sqe));
    io_uring_prep_uring_cmd(sqe, IORING_OP_URING_CMD, fd);
    sqe->cmd_op = UBLK_U_CMD_TRY_STOP_DEV;
    memcpy(sqe->cmd, &cmd, sizeof(cmd));
    ret = io_uring_submit(&ring);
    if (ret != 1) {
        fprintf(stderr, "io_uring_submit: expected 1, got %d\n", ret);
        goto out_ring;
    }
    ret = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe_timeout: %s\n", strerror(-ret));
        goto out_ring;
    }
    ret = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    printf("UBLK_TRY_STOP_PROBE expected=%s result=%d\n", expected, ret);
    if ((busy && ret == -EBUSY) || (!busy && ret == 0))
        status = 0;
    else
        fprintf(stderr, "unexpected TRY_STOP result (expected %s)\n", expected);

out_ring:
    io_uring_queue_exit(&ring);
out_close:
    if (close(fd) != 0) {
        perror("close /dev/ublk-control");
        status = 1;
    }
    return status;
}
