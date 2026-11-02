#ifndef BACKING_TOPOLOGY_H
#define BACKING_TOPOLOGY_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <linux/btrfs.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <stdlib.h>

#ifndef BACKING_IOCTL
#define BACKING_IOCTL ioctl
#endif
#ifndef BACKING_STAT
#define BACKING_STAT stat
#endif
#ifndef BACKING_REALPATH
#define BACKING_REALPATH realpath
#endif

static int backing_same_fs_info(const struct btrfs_ioctl_fs_info_args *a,
                                const struct btrfs_ioctl_fs_info_args *b)
{
    return !memcmp(a->fsid, b->fsid, sizeof(a->fsid)) &&
           a->num_devices == b->num_devices && a->max_id == b->max_id &&
           a->sectorsize == b->sectorsize;
}

/* A successful result is a stable observed snapshot, not an atomic guarantee. */
static int backing_btrfs_topology(int fd)
{
    struct btrfs_ioctl_fs_info_args first = {0}, second = {0};
    unsigned int matched = 0;

    if (BACKING_IOCTL(fd, BTRFS_IOC_FS_INFO, &first) < 0 ||
        first.num_devices < 1 || first.num_devices > 64 ||
        first.max_id < 1 || first.max_id > 4096)
        return -1;

    for (unsigned long long id = 1; id <= first.max_id; ++id) {
        struct btrfs_ioctl_dev_info_args dev = {0};
        struct stat st;
        char link[64], resolved[PATH_MAX];
        dev.devid = id;
        if (BACKING_IOCTL(fd, BTRFS_IOC_DEV_INFO, &dev) < 0) {
            if (errno == ENODEV) continue;
            return -1;
        }
        if (!memchr(dev.path, '\0', sizeof(dev.path)) || !dev.path[0] ||
            BACKING_STAT((const char *)dev.path, &st) < 0 || !S_ISBLK(st.st_mode))
            return -1;
        int n = snprintf(link, sizeof(link), "/sys/dev/block/%u:%u",
                         major(st.st_rdev), minor(st.st_rdev));
        if (n < 0 || (size_t)n >= sizeof(link) ||
            !BACKING_REALPATH(link, resolved) ||
            strncmp(resolved, "/sys/devices/", 13) ||
            !strncmp(resolved, "/sys/devices/virtual/", 21))
            return -1;
        ++matched;
    }
    if (matched != first.num_devices ||
        BACKING_IOCTL(fd, BTRFS_IOC_FS_INFO, &second) < 0 ||
        !backing_same_fs_info(&first, &second))
        return -1;
    return 0;
}
#endif
