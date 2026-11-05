#ifndef UBLK_NOTIFY_H
#define UBLK_NOTIFY_H

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int ublk_notify_ready(void)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t path_len, addr_len;
    int fd, ret, saved_errno;
    static const char msg[] = "READY=1\n";

    if (!path)
        return 0;
    path_len = strlen(path);
    if (!path_len || path_len >= sizeof(addr.sun_path) ||
        (path[0] != '/' && path[0] != '@')) {
        errno = EINVAL;
        return -1;
    }
    if (path[0] == '@') {
        if (path_len == 1) {
            errno = EINVAL;
            return -1;
        }
        memcpy(addr.sun_path, path, path_len);
        addr.sun_path[0] = '\0';
        addr_len = offsetof(struct sockaddr_un, sun_path) + path_len;
    } else {
        memcpy(addr.sun_path, path, path_len + 1);
        addr_len = offsetof(struct sockaddr_un, sun_path) + path_len + 1;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    do {
        ret = (int)sendto(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL,
                          (struct sockaddr *)&addr, (socklen_t)addr_len);
    } while (ret < 0 && errno == EINTR);
    saved_errno = ret < 0 ? errno : 0;
    if (close(fd) < 0 && ret >= 0) {
        saved_errno = errno;
        ret = -1;
    }
    if (ret < 0) {
        errno = saved_errno;
        return -1;
    }
    if ((size_t)ret != sizeof(msg) - 1) {
        errno = EIO;
        return -1;
    }
    return 0;
}

#endif
