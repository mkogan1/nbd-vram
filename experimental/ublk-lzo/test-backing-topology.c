#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/btrfs.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int fake_ioctl(int, unsigned long, void *);
static int fake_stat(const char *, struct stat *);
static char *fake_realpath(const char *, char *);
#define BACKING_IOCTL fake_ioctl
#define BACKING_STAT fake_stat
#define BACKING_REALPATH fake_realpath
#include "backing-topology.h"

enum { OK, FS_ERROR, DEV_ERROR, EMPTY, UNTERMINATED, STAT_ERROR, NONBLOCK,
       REALPATH_ERROR, COUNT_BAD, CHANGE, BOUND_LOW, BOUND_HIGH, ENODEV_HOLE };
enum { DIRECT, PARTITION, PCI_VIRTIO, VIRTUAL_LOOP, VIRTUAL_NBD,
       VIRTUAL_UBLK, VIRTUAL_DM };
static int scenario, kind, calls, fs_calls;

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; ++calls;
    if (request == BTRFS_IOC_FS_INFO) {
        struct btrfs_ioctl_fs_info_args *f = arg;
        ++fs_calls;
        if (scenario == FS_ERROR && fs_calls == 1) { errno = EIO; return -1; }
        memset(f, 0, sizeof(*f)); memset(f->fsid, 0x42, sizeof(f->fsid));
        f->num_devices = scenario == COUNT_BAD ? 2 : 1;
        f->max_id = scenario == BOUND_LOW ? 0 : scenario == BOUND_HIGH ? 4097 : 1;
        f->sectorsize = 4096;
        if (scenario == CHANGE && fs_calls == 2) f->sectorsize++;
        return 0;
    }
    if (request == BTRFS_IOC_DEV_INFO) {
        struct btrfs_ioctl_dev_info_args *d = arg;
        if (scenario == DEV_ERROR) { errno = EIO; return -1; }
        if (scenario == ENODEV_HOLE) { errno = ENODEV; return -1; }
        if (scenario == EMPTY) { d->path[0] = 0; return 0; }
        if (scenario == UNTERMINATED) { memset(d->path, 'x', sizeof(d->path)); return 0; }
        strcpy((char *)d->path, "/dev/fake"); return 0;
    }
    assert(0); return -1;
}
static int fake_stat(const char *path, struct stat *st)
{
    (void)path;
    if (scenario == STAT_ERROR) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = scenario == NONBLOCK ? S_IFREG : S_IFBLK;
    st->st_rdev = makedev(8, 1); return 0;
}
static char *fake_realpath(const char *path, char *resolved)
{
    (void)path;
    if (scenario == REALPATH_ERROR) { errno = ENOENT; return NULL; }
    const char *s = kind == DIRECT ? "/sys/devices/pci0000:00/block/nvme0n1" :
        kind == PARTITION ? "/sys/devices/pci0000:00/block/nvme0n1/nvme0n1p1" :
        kind == PCI_VIRTIO ? "/sys/devices/pci0000:00/virtio0/block/vda" :
        kind == VIRTUAL_LOOP ? "/sys/devices/virtual/block/loop0" :
        kind == VIRTUAL_NBD ? "/sys/devices/virtual/block/nbd0" :
        kind == VIRTUAL_UBLK ? "/sys/devices/virtual/block/ublkb0" :
        "/sys/devices/virtual/block/dm-0";
    strcpy(resolved, s); return resolved;
}
static void run(int sc, int k, int expect)
{
    scenario = sc; kind = k; calls = fs_calls = 0;
    assert((backing_btrfs_topology(7) == 0) == expect);
}
int main(void)
{
    run(OK, DIRECT, 1); run(OK, PARTITION, 1); run(OK, PCI_VIRTIO, 1);
    run(OK, VIRTUAL_LOOP, 0); run(OK, VIRTUAL_NBD, 0);
    run(OK, VIRTUAL_UBLK, 0); run(OK, VIRTUAL_DM, 0);
    run(EMPTY, DIRECT, 0); run(UNTERMINATED, DIRECT, 0);
    run(ENODEV_HOLE, DIRECT, 0); run(COUNT_BAD, DIRECT, 0);
    run(CHANGE, DIRECT, 0); run(FS_ERROR, DIRECT, 0);
    run(DEV_ERROR, DIRECT, 0); run(BOUND_LOW, DIRECT, 0);
    run(BOUND_HIGH, DIRECT, 0); run(STAT_ERROR, DIRECT, 0);
    run(NONBLOCK, DIRECT, 0); run(REALPATH_ERROR, DIRECT, 0);
    puts("backing topology tests passed");
}
