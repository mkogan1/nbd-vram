/* Mock-only inclusion tests for init_disk_reserve/check_disk_reserve.
 * cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 *   -isystem /var/tmp/ublk-compare-src/include -isystem /var/tmp/ublk-compare-src \
 *   test-ublk-disk-reserve.c -Wl,--gc-sections -o /var/tmp/test-ublk-disk-reserve
 * /var/tmp/test-ublk-disk-reserve
 * No target main, libraries, real files, devices, GPU or swap are used.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

static int fixture_fstatfs(int, struct statfs *);
static int fixture_fstatvfs(int, struct statvfs *);
static int fixture_fstat(int, struct stat *);
static int fixture_open(const char *, int, ...);
static int fixture_fallocate(int, int, off_t, off_t);
static int fixture_close(int);
static int fixture_syncfs(int);
static int fixture_printf(const char *, ...);
static int fixture_fprintf(FILE *, const char *, ...);
static int fixture_puts(const char *);
int main(int, char **) __asm__("unused_ublk_target_main");
#define fstatfs fixture_fstatfs
#define fstatvfs fixture_fstatvfs
#define fstat fixture_fstat
#define open fixture_open
#define fallocate fixture_fallocate
#define close fixture_close
#define syncfs fixture_syncfs
#define printf fixture_printf
#define fprintf fixture_fprintf
#define puts fixture_puts
#include "ublk-lzo-target.c"
#undef fstatfs
#undef fstatvfs
#undef fstat
#undef open
#undef fallocate
#undef close
#undef syncfs
#undef printf
#undef fprintf
#undef puts

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _Exit(90); \
} } while (0)

enum { BACK_FD = 41, RESERVE_FD = 42, QUERY_MAX = 8 };
static uint64_t space_blocks[QUERY_MAX];
static int space_errors[QUERY_MAX], space_count, space_calls;
static int fs_error, backing_fstat_error, reserve_fstat_error;
static long fs_type;
static int open_error, fallocate_error, close_error, sync_error;
static dev_t backing_dev = 17, reserve_dev = 17;
static mode_t reserve_mode = S_IFREG;
static nlink_t reserve_links;
static blkcnt_t reserve_blocks = (64 * 1024 * 1024) / 512;
static off_t allocated_bytes;
static int opens, fallocates, closes, syncs, backing_stats, reserve_stats;
static char log_buffer[4096];
static size_t log_size;

static void logv(const char *format, va_list ap)
{
    int n = vsnprintf(log_buffer + log_size, sizeof(log_buffer) - log_size, format, ap);
    CHECK(n >= 0 && (size_t)n < sizeof(log_buffer) - log_size);
    log_size += (size_t)n;
}

static int fixture_printf(const char *format, ...)
{
    va_list ap; va_start(ap, format); logv(format, ap); va_end(ap); return 0;
}

static int fixture_fprintf(FILE *stream, const char *format, ...)
{
    CHECK(stream == stdout || stream == stderr);
    va_list ap; va_start(ap, format); logv(format, ap); va_end(ap); return 0;
}

static int fixture_puts(const char *s)
{
    fixture_printf("%s\n", s); return 0;
}

static int fixture_fstatfs(int fd, struct statfs *fs)
{
    CHECK(fd == BACK_FD);
    if (fs_error) { errno = fs_error; return -1; }
    memset(fs, 0, sizeof(*fs)); fs->f_type = fs_type; return 0;
}

static int fixture_fstatvfs(int fd, struct statvfs *space)
{
    CHECK(fd == BACK_FD && space_calls < space_count);
    int i = space_calls++;
    if (space_errors[i]) { errno = space_errors[i]; return -1; }
    memset(space, 0, sizeof(*space));
    space->f_frsize = 1024 * 1024; space->f_bavail = space_blocks[i]; return 0;
}

static int fixture_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd == BACK_FD) {
        ++backing_stats;
        if (backing_fstat_error) { errno = backing_fstat_error; return -1; }
        st->st_dev = backing_dev; st->st_mode = S_IFREG; st->st_nlink = 1; return 0;
    }
    CHECK(fd == RESERVE_FD); ++reserve_stats;
    if (reserve_fstat_error) { errno = reserve_fstat_error; return -1; }
    st->st_dev = reserve_dev; st->st_mode = reserve_mode; st->st_nlink = reserve_links;
    st->st_blocks = reserve_blocks; return 0;
}

static int fixture_open(const char *path, int flags, ...)
{
    CHECK(!strcmp(path, "/mock/backing-dir"));
    CHECK(flags == (O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC));
    ++opens;
    if (open_error) { errno = open_error; return -1; }
    return RESERVE_FD;
}

static int fixture_fallocate(int fd, int mode, off_t offset, off_t bytes)
{
    CHECK(fd == RESERVE_FD && !mode && !offset); ++fallocates;
    if (fallocate_error) { errno = fallocate_error; return -1; }
    allocated_bytes = bytes; return 0;
}

static int fixture_close(int fd)
{
    CHECK(fd == RESERVE_FD); ++closes;
    if (close_error) { errno = close_error; return -1; }
    return 0;
}

static int fixture_syncfs(int fd)
{
    CHECK(fd == BACK_FD); ++syncs;
    if (sync_error) { errno = sync_error; return -1; }
    return 0;
}

static void reset(void)
{
    memset(space_blocks, 0, sizeof(space_blocks));
    memset(space_errors, 0, sizeof(space_errors));
    space_count = space_calls = fs_error = 0; fs_type = 0;
    backing_fstat_error = reserve_fstat_error = open_error = 0;
    fallocate_error = close_error = sync_error = 0;
    backing_dev = reserve_dev = 17; reserve_mode = S_IFREG; reserve_links = 0;
    reserve_blocks = (64 * 1024 * 1024) / 512; allocated_bytes = 0;
    opens = fallocates = closes = syncs = backing_stats = reserve_stats = 0;
    log_size = 0; log_buffer[0] = '\0'; g_back_fd = BACK_FD;
}

static void btrfs_queries(uint64_t first, uint64_t second)
{
    fs_type = BTRFS_SUPER_MAGIC;
    space_count = 2; space_blocks[0] = first; space_blocks[1] = second;
}

static void test_filesystem_gate_and_query_errors(void)
{
    struct queue q = { .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    reset(); fs_type = EXT4_SUPER_MAGIC;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 0 && space_calls == 0 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_NOT_APPLICABLE"));

    reset(); fs_type = BTRFS_SUPER_MAGIC; space_count = 1; space_errors[0] = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 0 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_UNAVAILABLE errno=5"));

    reset(); q.spare_fd = RESERVE_FD; q.reserve_state = DISK_RESERVE_READY;
    space_count = 2; space_errors[0] = space_errors[1] = EIO;
    check_disk_reserve(&q); check_disk_reserve(&q);
    CHECK(q.spare_fd == RESERVE_FD && q.reserve_state == DISK_RESERVE_READY);
    CHECK(closes == 0 && syncs == 0 && space_calls == 2);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_QUERY_FAILED errno=5"));
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_FAILED") == NULL);
    const char *query_log = "UBLK_DISK_RESERVE_QUERY_FAILED";
    char *first = strstr(log_buffer, query_log);
    CHECK(first && !strstr(first + strlen(query_log), query_log));
}

static void test_thresholds_and_filesystem_ownership(void)
{
    struct queue q = { .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    reset(); btrfs_queries(128, 64);
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(q.spare_fd == RESERVE_FD && q.reserve_state == DISK_RESERVE_READY);
    CHECK(opens == 1 && fallocates == 1 && allocated_bytes == 64LL * 1024 * 1024);
    CHECK(backing_stats == 1 && reserve_stats == 1 && closes == 0);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_READY bytes=67108864"));

    reset(); q = (struct queue){ .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    btrfs_queries(127, 64);
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 0 && space_calls == 1 && q.reserve_state == DISK_RESERVE_OFF);

    reset(); q = (struct queue){ .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    fs_type = BTRFS_SUPER_MAGIC; space_count = 3;
    space_blocks[0] = 128; space_blocks[1] = 63; space_blocks[2] = 64;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 1 && closes == 1 && space_calls == 3 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "reason=headroom_not_preserved"));

    reset(); q = (struct queue){ .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    fs_type = BTRFS_SUPER_MAGIC; space_count = 3;
    space_blocks[0] = 128; space_blocks[1] = 63; space_blocks[2] = 63;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == -1);
    CHECK(opens == 1 && closes == 1 && space_calls == 3 && q.reserve_state == DISK_RESERVE_OFF);
}

static void test_reserve_creation_failures_cleanup(void)
{
    struct queue q = { .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };

    reset(); btrfs_queries(128, 64); open_error = EACCES;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 1 && closes == 0 && q.reserve_state == DISK_RESERVE_OFF);

    reset(); btrfs_queries(128, 64); fallocate_error = ENOSPC;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(closes == 1 && syncs == 0 && q.reserve_state == DISK_RESERVE_OFF);

    reset(); btrfs_queries(128, 64); reserve_fstat_error = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(closes == 1 && q.reserve_state == DISK_RESERVE_OFF);

    reset(); btrfs_queries(128, 64); reserve_dev++;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(closes == 1 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_UNAVAILABLE errno=5"));

    reset(); btrfs_queries(128, 64); reserve_links = 1;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(closes == 1 && q.reserve_state == DISK_RESERVE_OFF);
}

static void test_preflight_errors_and_post_reserve_headroom(void)
{
    struct queue q = { .spare_fd = -1, .reserve_state = DISK_RESERVE_OFF };
    reset(); fs_error = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 0 && space_calls == 0);

    reset(); fs_type = BTRFS_SUPER_MAGIC; backing_fstat_error = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(opens == 0 && space_calls == 0);

    reset(); fs_type = BTRFS_SUPER_MAGIC; space_count = 3;
    space_blocks[0] = 128; space_blocks[2] = 64; space_errors[1] = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == 0);
    CHECK(closes == 1 && space_calls == 3 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "reason=headroom_query_failed"));

    reset(); fs_type = BTRFS_SUPER_MAGIC; space_count = 3;
    space_blocks[0] = 128; space_errors[1] = EIO; space_errors[2] = EIO;
    CHECK(init_disk_reserve(&q, "/mock/backing-dir") == -1);
    CHECK(closes == 1 && space_calls == 3 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(strstr(log_buffer, "cannot verify Btrfs headroom after reserve attempt"));
}

static void test_release_once_and_failed_release_no_retry(void)
{
    struct queue q = { .spare_fd = RESERVE_FD, .reserve_state = DISK_RESERVE_READY };
    reset(); space_count = 3; space_blocks[0] = 64; space_blocks[1] = 0; space_blocks[2] = 0;
    check_disk_reserve(&q);
    CHECK(q.reserve_state == DISK_RESERVE_READY && q.spare_fd == RESERVE_FD);
    CHECK(closes == 0 && syncs == 0);
    check_disk_reserve(&q);
    CHECK(q.reserve_state == DISK_RESERVE_ATTEMPTED && q.spare_fd == -1);
    CHECK(closes == 1 && syncs == 1 && space_calls == 2);
    CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_RELEASED errno=0"));
    check_disk_reserve(&q);
    CHECK(closes == 1 && syncs == 1 && space_calls == 2);

    for (int fail_close = 0; fail_close <= 1; ++fail_close) {
        reset(); q = (struct queue){ .spare_fd = RESERVE_FD, .reserve_state = DISK_RESERVE_READY };
        space_count = 1; space_blocks[0] = 0;
        close_error = fail_close ? EIO : 0; sync_error = fail_close ? 0 : EIO;
        check_disk_reserve(&q); check_disk_reserve(&q);
        CHECK(q.reserve_state == DISK_RESERVE_ATTEMPTED && q.spare_fd == -1);
        CHECK(closes == 1 && syncs == 1 && space_calls == 1);
        CHECK(strstr(log_buffer, "UBLK_DISK_RESERVE_FAILED errno=5"));
    }
}

static void test_fd_zero_is_never_closed_when_absent(void)
{
    struct queue q = { .spare_fd = 0, .reserve_state = DISK_RESERVE_OFF };
    reset(); check_disk_reserve(&q);
    CHECK(q.spare_fd == 0 && q.reserve_state == DISK_RESERVE_OFF);
    CHECK(closes == 0 && syncs == 0 && space_calls == 0);
    q.reserve_state = DISK_RESERVE_ATTEMPTED; check_disk_reserve(&q);
    CHECK(closes == 0 && syncs == 0 && space_calls == 0);
}

int disk_reserve_test_main(void) __asm__("main");
int disk_reserve_test_main(void)
{
    test_filesystem_gate_and_query_errors();
    test_thresholds_and_filesystem_ownership();
    test_reserve_creation_failures_cleanup();
    test_preflight_errors_and_post_reserve_headroom();
    test_release_once_and_failed_release_no_retry();
    test_fd_zero_is_never_closed_when_absent();
    puts("PASS disk reserve mocks");
    return 0;
}
