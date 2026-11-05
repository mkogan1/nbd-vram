/* CPU-only backing_init tests: filesystem operations are mocked; no mounts or swap. */
#ifdef NDEBUG
#error This test requires assertions enabled
#endif
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>

static ssize_t mock_pwrite(int, const void *, size_t, off_t);
static int mock_fsync(int);
static int mock_open(const char *, int, ...);
static int mock_fstatfs(int, struct statfs *);
static int mock_fstatvfs(int, struct statvfs *);
static int mock_fstat(int, struct stat *);
static int mock_statx(int, const char *, int, unsigned, struct statx *);
static int mock_ioctl(int, unsigned long, void *);
static int mock_fallocate(int, int, off_t, off_t);
static int mock_stat(const char *, struct stat *);
static char *mock_realpath(const char *, char *);
#define stat(...) mock_stat(__VA_ARGS__)
#define realpath mock_realpath
#define pwrite mock_pwrite
#define fsync mock_fsync
#define open mock_open
#define fstatfs mock_fstatfs
#define fstatvfs mock_fstatvfs
#define fstat mock_fstat
#define statx(...) mock_statx(__VA_ARGS__)
#define ioctl(fd, request, arg) mock_ioctl(fd, request, (void *)(arg))
#define fallocate mock_fallocate
#include "test-metadata.h"
#undef pwrite
#undef fsync
#undef open
#undef fstatfs
#undef fstatvfs
#undef fstat
#undef statx
#undef ioctl
#undef fallocate
#undef stat
#undef realpath

static long fs_type;
static unsigned sector, mem_align, offset_align, alignment_mask;
static int fd_seen, inode_flags, info_error, statx_error, info_calls, statx_calls;
static int preallocated, fail_setflags, fail_allocate;
static int write_mode, write_calls, sync_mode, sync_calls;
static int space_error, space_calls;
static fsblkcnt_t available_blocks = 64 * 1024 * 1024 / 4096;
static unsigned long fragment_size = 4096;
static off_t written;

static int mock_fstatvfs(int fd, struct statvfs *space)
{
    assert(fd == fd_seen && g_back_fd == -1 && preallocated);
    assert(fs_type == BTRFS_SUPER_MAGIC && written == (off_t)(2 * g_export_size));
    assert(sync_mode != 1 && sync_calls == (sync_mode == 2 ? 2 : 1));
    space_calls++;
    if (space_error) { errno = EIO; return -1; }
    memset(space, 0, sizeof(*space));
    space->f_bavail = available_blocks;
    space->f_frsize = fragment_size;
    return 0;
}

static ssize_t mock_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    assert(fd == fd_seen && g_back_fd == -1 && preallocated);
    assert(fs_type == BTRFS_SUPER_MAGIC && (inode_flags & FS_NOCOW_FL));
    assert(info_calls == 3 && !statx_calls && !sync_calls);
    assert(offset == written && count > 0 && count <= COMP_PAGE);
    assert((uintptr_t)buf % sector == 0 && offset % sector == 0 && count % sector == 0);
    assert(offset + (off_t)count <= (off_t)(2 * g_export_size));
    for (size_t i = 0; i < count; i++) assert(((const char *)buf)[i] == 0);
    write_calls++;
    if (write_mode == 1 && write_calls == 1) { errno = EINTR; return -1; }
    if ((write_mode == 2 || write_mode == 6) && write_calls == 1) count = sector;
    if (write_mode == 3 && write_calls == 2) { errno = ENOSPC; return -1; }
    if (write_mode == 6 && write_calls == 2) { errno = EIO; return -1; }
    if (write_mode == 4) return 0;
    if (write_mode == 5) return 1; /* Unaligned progress cannot be resumed with O_DIRECT. */
    written += count;
    return (ssize_t)count;
}
static int mock_fsync(int fd)
{
    assert(fd == fd_seen && g_back_fd == -1 && written == (off_t)(2 * g_export_size));
    sync_calls++;
    if (sync_mode == 1) { errno = EIO; return -1; }
    if (sync_mode == 2 && sync_calls == 1) { errno = EINTR; return -1; }
    return 0;
}

static int mock_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "mock") && (flags & O_TMPFILE) == O_TMPFILE && (flags & O_DIRECT));
    fd_seen = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd_seen >= 0);
    return fd_seen;
}
static int mock_fstatfs(int fd, struct statfs *fs)
{
    assert(fd == fd_seen);
    memset(fs, 0, sizeof(*fs));
    fs->f_type = fs_type;
    return 0;
}
static int mock_fstat(int fd, struct stat *st)
{
    assert(fd == fd_seen);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    if (preallocated) {
        st->st_size = 2 * g_export_size;
        st->st_blocks = st->st_size / 512;
    }
    return 0;
}
static int mock_statx(int fd, const char *path, int flags, unsigned mask, struct statx *st)
{
    assert(fd == fd_seen && !*path && flags == AT_EMPTY_PATH && mask == STATX_DIOALIGN);
    statx_calls++;
    memset(st, 0, sizeof(*st));
    st->stx_mask = alignment_mask;
    st->stx_dio_mem_align = mem_align;
    st->stx_dio_offset_align = offset_align;
    if (statx_error) { errno = EIO; return -1; }
    return 0;
}
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    assert(fd == fd_seen && fs_type == BTRFS_SUPER_MAGIC);
    if (request == BTRFS_IOC_DEV_INFO) {
        struct btrfs_ioctl_dev_info_args *dev = arg;
        assert(dev->devid == 1);
        strcpy((char *)dev->path, "/dev/test-backing");
        return 0;
    }
    if (request == FS_IOC_GETFLAGS) { *(int *)arg = inode_flags; return 0; }
    if (request == FS_IOC_SETFLAGS) {
        if (fail_setflags) { errno = EIO; return -1; }
        inode_flags = *(int *)arg;
        return 0;
    }
    assert(request == BTRFS_IOC_FS_INFO);
    struct btrfs_ioctl_fs_info_args zero = {0};
    assert(!memcmp(arg, &zero, sizeof(zero)));
    info_calls++;
    if (info_error) { errno = EIO; return -1; }
    ((struct btrfs_ioctl_fs_info_args *)arg)->sectorsize = sector;
    ((struct btrfs_ioctl_fs_info_args *)arg)->num_devices = 1;
    ((struct btrfs_ioctl_fs_info_args *)arg)->max_id = 1;
    return 0;
}
static int mock_stat(const char *path, struct stat *st)
{
    assert(!strcmp(path, "/dev/test-backing"));
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFBLK;
    st->st_rdev = makedev(259, 2);
    return 0;
}
static char *mock_realpath(const char *path, char *resolved)
{
    assert(!strcmp(path, "/sys/dev/block/259:2"));
    strcpy(resolved, "/sys/devices/pci0000:00/nvme/nvme1n1/nvme1n1p2");
    return resolved;
}
static int mock_fallocate(int fd, int mode, off_t offset, off_t bytes)
{
    assert(fd == fd_seen && !mode && !offset && bytes == (off_t)(2 * g_export_size));
    if (fail_allocate) { errno = ENOSPC; return -1; }
    preallocated = 1;
    return 0;
}
static void check(int success)
{
    inode_flags = preallocated = info_calls = statx_calls = 0;
    written = write_calls = sync_calls = space_calls = 0;
    memset(t_disk_page, 0x5a, sizeof(t_disk_page));
    assert(g_back_fd == -1);
    assert((backing_init("mock") == 0) == success);
    if (success) {
        assert(g_back_fd == fd_seen && preallocated);
        if (fs_type == BTRFS_SUPER_MAGIC) {
            assert(written == (off_t)(2 * g_export_size));
            assert(sync_calls == (sync_mode == 2 ? 2 : 1));
            assert(space_calls == 1);
        }
        assert(close(g_back_fd) == 0);
        g_back_fd = -1;
    } else {
        assert(g_back_fd == -1);
        errno = 0;
        assert(fcntl(fd_seen, F_GETFD) == -1 && errno == EBADF);
    }
    assert(statx_calls == (fs_type != BTRFS_SUPER_MAGIC));
    if (fs_type != BTRFS_SUPER_MAGIC || !preallocated) assert(!write_calls && !sync_calls);
    if (!success && write_mode) assert(!sync_calls);
    if (fs_type != BTRFS_SUPER_MAGIC || written != (off_t)(2 * g_export_size) ||
        !sync_calls || sync_mode == 1) assert(!space_calls);
}
int main(void)
{
    g_compress = COMP_LZ4;
    g_export_size = 32 * COMP_PAGE;
    fs_type = BTRFS_SUPER_MAGIC;
    /* Missing statx alignment must not prevent verified Btrfs sector alignment. */
    sector = COMP_PAGE; check(1); assert(info_calls == 3);
    sector = 512; check(1);
    for (write_mode = 1; write_mode <= 6; write_mode++) check(write_mode <= 2);
    write_mode = 0;
    sync_mode = 1; check(0);
    sync_mode = 2; check(1);
    sync_mode = 0;
    /* Exact 64 MiB boundary, one block below, and invalid/query-failure results. */
    check(1);
    available_blocks--; check(0); assert(space_calls == 1);
    available_blocks++; fragment_size = 0; check(0); assert(space_calls == 1);
    fragment_size = 4096; space_error = 1; check(0); assert(space_calls == 1);
    space_error = 0;
    available_blocks = 0; check(0);
    /* Non-divisible fragment size must round the required block count up. */
    fragment_size = 4097; available_blocks = (64ULL * 1024 * 1024) / fragment_size;
    check(0); available_blocks++; check(1);
    /* Multiplication would wrap: the division comparison still accepts these. */
    fragment_size = 4096; available_blocks = (fsblkcnt_t)-1; check(1);
    fragment_size = ULONG_MAX; available_blocks = 1; check(1);
    fragment_size = 4096; available_blocks = 64 * 1024 * 1024 / 4096;
    info_error = 1; check(0); assert(!preallocated); info_error = 0;
    sector = 0; check(0); assert(!preallocated);
    sector = 3072; check(0); assert(!preallocated);
    sector = 2 * COMP_PAGE; check(0); assert(!preallocated);
    sector = COMP_PAGE;
    fail_setflags = 1; check(0); assert(info_calls == 2); fail_setflags = 0;
    fail_allocate = 1; check(0); fail_allocate = 0;
    space_error = 1; fragment_size = 0; /* ext4/XFS must never query headroom. */
    for (int i = 0; i < 2; i++) {
        fs_type = i ? XFS_SUPER_MAGIC : EXT4_SUPER_MAGIC;
        alignment_mask = STATX_DIOALIGN;
        mem_align = offset_align = COMP_PAGE;
        check(1); assert(!info_calls);
        statx_error = 1; check(0); statx_error = 0;
        alignment_mask = 0; check(0); alignment_mask = STATX_DIOALIGN;
        mem_align = 0; check(0); mem_align = COMP_PAGE;
        offset_align = 0; check(0); offset_align = COMP_PAGE;
        mem_align = 2 * COMP_PAGE; check(0); mem_align = COMP_PAGE;
        offset_align = 3072; check(0); offset_align = COMP_PAGE;
    }
    puts("PASS backing alignment: Btrfs alignment, prewrite/fsync, 64 MiB headroom boundaries/errors/overflow, fd cleanup, ext4/XFS unchanged");
    return 0;
}
