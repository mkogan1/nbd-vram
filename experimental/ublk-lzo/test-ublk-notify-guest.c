#define _GNU_SOURCE
#include "ublk-guest-guard.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char *end, buf[64];
    unsigned long long expected;
    uint64_t size = 0;
    int fd, disk, rc = 1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX,
                               .sun_path = "/tmp/ublk-notify.sock" };
    if (ublk_guest_guard() || argc != 2) return 1;
    errno = 0;
    expected = strtoull(argv[1], &end, 10);
    if (errno || !*argv[1] || *end || !expected) return 1;
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return 1;
    /* Do not replace an existing listener, even in the disposable guest. */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) goto out;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 20000) != 1 || !(p.revents & POLLIN)) goto out;
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n != 8 || memcmp(buf, "READY=1\n", 8)) goto out;
    disk = open("/dev/ublkb0", O_RDONLY | O_CLOEXEC);
    if (disk < 0) goto out;
    int valid = ioctl(disk, BLKGETSIZE64, &size) == 0 && size == expected;
    if (close(disk) || !valid) goto out;
    puts("INTEGRATION_NOTIFY_READY_PASS");
    rc = 0;
out:
    close(fd);
    return rc;
}
