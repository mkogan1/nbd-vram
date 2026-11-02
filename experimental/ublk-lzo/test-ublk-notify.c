#define _GNU_SOURCE
#ifdef NDEBUG
#error This test requires assertions enabled
#endif
#include "ublk-notify.h"

#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

static void receive_ready(int fd)
{
    char buf[32];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);

    assert(n == 8);
    assert(memcmp(buf, "READY=1\n", 8) == 0);
}

int main(void)
{
    char dir[] = "/tmp/ublk-notify-XXXXXX";
    char pathname[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    socklen_t addr_len;
    int fd;

    assert(unsetenv("NOTIFY_SOCKET") == 0);
    assert(ublk_notify_ready() == 0);

    assert(mkdtemp(dir) != NULL);
    snprintf(pathname, sizeof(pathname), "%s/sock", dir);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    memcpy(addr.sun_path, pathname, strlen(pathname) + 1);
    addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                           strlen(pathname) + 1);
    assert(bind(fd, (struct sockaddr *)&addr, addr_len) == 0);
    assert(setenv("NOTIFY_SOCKET", pathname, 1) == 0);
    assert(ublk_notify_ready() == 0);
    receive_ready(fd);
    /* A full notification queue must not block startup indefinitely. */
    alarm(5);
    int sent;
    for (sent = 0; sent < 1024; sent++)
        if (ublk_notify_ready() != 0) break;
    assert(sent < 1024 && (errno == EAGAIN || errno == EWOULDBLOCK));
    alarm(0);
    close(fd);
    assert(unlink(pathname) == 0);

#ifdef __linux__
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(pathname, sizeof(pathname), "@ublk-notify-test-%ld", (long)getpid());
    memcpy(addr.sun_path + 1, pathname + 1, strlen(pathname) - 1);
    addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                           strlen(pathname));
    assert(bind(fd, (struct sockaddr *)&addr, addr_len) == 0);
    assert(setenv("NOTIFY_SOCKET", pathname, 1) == 0);
    assert(ublk_notify_ready() == 0);
    receive_ready(fd);
    close(fd);
#endif

    assert(setenv("NOTIFY_SOCKET", "", 1) == 0);
    assert(ublk_notify_ready() == -1);
    assert(setenv("NOTIFY_SOCKET", "relative", 1) == 0);
    assert(ublk_notify_ready() == -1);
    assert(setenv("NOTIFY_SOCKET", "/", 1) == 0);
    assert(ublk_notify_ready() == -1);
    memset(pathname, 'x', sizeof(((struct sockaddr_un *)0)->sun_path));
    pathname[sizeof(((struct sockaddr_un *)0)->sun_path)] = '\0';
    assert(setenv("NOTIFY_SOCKET", pathname, 1) == 0);
    assert(ublk_notify_ready() == -1);
    assert(setenv("NOTIFY_SOCKET", "/tmp/ublk-notify-no-such-socket", 1) == 0);
    assert(ublk_notify_ready() == -1);

    assert(unsetenv("NOTIFY_SOCKET") == 0);
    assert(rmdir(dir) == 0);
    puts("ublk notify tests passed");
    return 0;
}
