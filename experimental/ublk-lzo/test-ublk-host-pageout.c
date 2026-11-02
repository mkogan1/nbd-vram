/* Host-only preparatory pageout handshake. Never activates swap or changes cgroups.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -I$UBLK_SRC/include -I$UBLK_SRC test-ublk-host-pageout.c -L$UBLK_SRC/lib/.libs -Wl,-rpath,$UBLK_SRC/lib/.libs -lublksrv -luring -ldl -pthread -o /tmp/test-ublk-host-pageout
 * Run only --self-test unless an external harness deliberately starts the normal protocol.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/ublk_cmd.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/random.h>
#include <linux/magic.h>
#include <time.h>
#include <unistd.h>

#include "ublksrv.h"

#define MAP_BYTES (16U * 1024U * 1024U)
#define PAGE_BYTES 4096U
#define SWAPPED (UINT64_C(1) << 62)
#define PRESENT (UINT64_C(1) << 63)
#define DEVICE_BYTES (UINT64_C(128) * 1024 * 1024)
#define UBLK_ID 31
#define CGROUP_FILE_BYTES 4096U

static int decimal(const char *s, uintmax_t *out)
{
    if (!s || !*s) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    uintmax_t n = strtoumax(s, &end, 10);
    if (errno || *end) return -1;
    *out = n;
    return 0;
}

static int bounded_file(const char *path, char *buf, size_t cap, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0; int rc = -1;
    while (used < cap) {
        ssize_t n = read(fd, buf + used, cap - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) { buf[used] = '\0'; *len = used; rc = 0; goto done; }
        used += (size_t)n;
    }
    errno = EOVERFLOW;
done:
    if (close(fd)) rc = -1;
    return rc;
}

static int cgroup_membership_matches(const char *text, pid_t pid)
{
    char expected[128];
    int n = snprintf(expected, sizeof(expected), "0::/vram-pageout-test-%jd", (intmax_t)pid);
    if (n < 0 || (size_t)n >= sizeof(expected)) return 0;
    const char *newline = strchr(text, '\n');
    return !strncmp(text, expected, (size_t)n) &&
           (!text[n] || (text[n] == '\n' && !newline[1]));
}

static int limit_matches(const char *actual, const char *expected)
{
    return !strcmp(actual, expected);
}

static int exact_cgroup_file(const char *dir, const char *name, const char *expected)
{
    char path[512], text[128]; size_t len;
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path) || bounded_file(path, text, sizeof(text) - 1, &len)) return -1;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = '\0';
    return limit_matches(text, expected) ? 0 : -1;
}

static int admit_cgroup(void)
{
    char membership[CGROUP_FILE_BYTES]; size_t len;
    if (geteuid() != 0) { fprintf(stderr, "admission: root required\n"); return -1; }
    if (bounded_file("/proc/self/cgroup", membership, sizeof(membership) - 1, &len) ||
        !cgroup_membership_matches(membership, getpid())) {
        fprintf(stderr, "admission: process is not in its exact private cgroup /vram-pageout-test-%jd\n", (intmax_t)getpid()); return -1;
    }
    char dir[128];
    int n = snprintf(dir, sizeof(dir), "/sys/fs/cgroup/vram-pageout-test-%jd", (intmax_t)getpid());
    struct stat st; struct statfs fs;
    if (n < 0 || (size_t)n >= sizeof(dir) || stat(dir, &st) || !S_ISDIR(st.st_mode) ||
        statfs(dir, &fs) || fs.f_type != CGROUP2_SUPER_MAGIC) {
        fprintf(stderr, "admission: private cgroup is not a cgroup v2 directory\n"); return -1;
    }
    if (exact_cgroup_file(dir, "memory.max", "67108864") ||
        exact_cgroup_file(dir, "memory.swap.max", "33554432") ||
        exact_cgroup_file(dir, "memory.zswap.max", "0") ||
        exact_cgroup_file(dir, "memory.zswap.writeback", "1")) {
        fprintf(stderr, "admission: cgroup memory/swap limits do not match required values\n"); return -1;
    }
    return 0;
}

static int pagemap_entry(uint64_t entry, uint64_t *swap_offset)
{
    if (!(entry & SWAPPED) || (entry & PRESENT)) return 0;
    if (swap_offset) *swap_offset = (entry >> 5) & ((UINT64_C(1) << 50) - 1);
    return 1;
}

static void page_data(unsigned char *out, unsigned page, uint64_t nonce)
{
    uint64_t x = nonce ^ ((uint64_t)page * UINT64_C(0x9e3779b97f4a7c15));
    if (!x) x = UINT64_C(0x2545f4914f6cdd1d);
    for (size_t i = 0; i < PAGE_BYTES; ++i) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        x *= UINT64_C(0x2545f4914f6cdd1d);
        out[i] = (unsigned char)x;
    }
    if (page % 4 == 0)
        for (size_t i = 32; i < PAGE_BYTES; ++i) out[i] = out[i % 32];
}

static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    int64_t ms = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
                 (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (ms <= 0) return 0;
    return ms > INT32_MAX ? INT32_MAX : (int)ms;
}

static int valid_info(struct ublksrv_ctrl_dev *ctrl, pid_t expected_pid)
{
    if (ublksrv_ctrl_get_info(ctrl) < 0) { fprintf(stderr, "fresh ublk get_info failed\n"); return -1; }
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(ctrl);
    if (!info || info->dev_id != UBLK_ID || info->state != UBLK_S_DEV_LIVE || info->ublksrv_pid != expected_pid) {
        fprintf(stderr, "ublk identity/state mismatch (id=%u state=%u pid=%d)\n", info ? info->dev_id : UINT32_MAX,
                info ? info->state : UINT16_MAX, info ? info->ublksrv_pid : -1); return -1;
    }
    return 0;
}

static int inspect_swapped(const unsigned char *mapping, size_t *pages, int devfd, uint64_t nonce)
{
    long ps = sysconf(_SC_PAGESIZE);
    if (ps != PAGE_BYTES) { fprintf(stderr, "unsupported page size\n"); return -1; }
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open pagemap"); return -1; }
    size_t count = 0, other_pages = 0; int rc = -1, matched_type = -1;
    uint32_t invalid_types = 0;
    unsigned char *actual = NULL;
    if (posix_memalign((void **)&actual, PAGE_BYTES, PAGE_BYTES)) { fprintf(stderr, "aligned page allocation failed\n"); goto done; }
    uintptr_t base = (uintptr_t)mapping;
    for (size_t i = 0; i < MAP_BYTES / PAGE_BYTES; ++i) {
        uint64_t entry; off_t off = (off_t)(((base / PAGE_BYTES) + i) * sizeof(entry));
        ssize_t got;
        do { got = pread(fd, &entry, sizeof(entry), off); } while (got < 0 && errno == EINTR);
        if (got != (ssize_t)sizeof(entry)) { if (got >= 0) errno = EIO; perror("read pagemap"); goto done; }
        uint64_t swap_offset;
        if (pagemap_entry(entry, &swap_offset)) {
            unsigned type = (unsigned)(entry & 31);
            int matches = 0;
            if (swap_offset >= DEVICE_BYTES / PAGE_BYTES || swap_offset > (uint64_t)INT64_MAX / PAGE_BYTES) {
                /* Existing per-CPU swap slots may belong to another swap device. */
                goto unmatched;
            }
            ssize_t n;
            do { n = pread(devfd, actual, PAGE_BYTES, (off_t)(swap_offset * PAGE_BYTES)); } while (n < 0 && errno == EINTR);
            unsigned char expected[PAGE_BYTES]; page_data(expected, (unsigned)i, nonce);
            matches = n == PAGE_BYTES && !memcmp(expected, actual, PAGE_BYTES);
unmatched:
            if (!matches) {
                invalid_types |= UINT32_C(1) << type;
                if (matched_type == (int)type) {
                    fprintf(stderr, "device attribution mismatch within verified swap type at page=%zu\n", i); goto done;
                }
                ++other_pages;
                continue;
            }
            if ((invalid_types & (UINT32_C(1) << type)) ||
                (matched_type >= 0 && matched_type != (int)type)) {
                fprintf(stderr, "inconsistent swap type attribution\n"); goto done;
            }
            matched_type = (int)type;
            ++count;
        }
    }
    if (count) printf("PAGEOUT_ATTRIBUTION type=%d verified=%zu other_swap_pages=%zu\n", matched_type, count, other_pages);
    *pages = count; rc = 0;
done:
    free(actual);
    if (close(fd)) { perror("close pagemap"); rc = -1; }
    return rc;
}

static int wait_ready(const unsigned char *mapping, int devfd, uint64_t nonce)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) { perror("clock_gettime"); return -1; }
    deadline.tv_sec += 10;
    size_t pages = 0;
    for (;;) {
        if (inspect_swapped(mapping, &pages, devfd, nonce)) return -1;
        if (pages) break;
        int ms = remaining_ms(&deadline);
        if (ms < 0) { perror("clock_gettime"); return -1; }
        if (!ms) { fprintf(stderr, "no swapped mapping pages before 10s deadline\n"); return -1; }
        struct timespec nap = { .tv_sec = 0, .tv_nsec = 50000000 };
        while (nanosleep(&nap, &nap)) {
            if (errno == EINTR) continue;
            perror("nanosleep"); return -1;
        }
    }
    printf("PAGEOUT_READY pages=%zu attributed=%zu\n", pages, pages); fflush(stdout);
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &end)) { perror("clock_gettime"); return -1; }
    end.tv_sec += 30;
    for (;;) {
        int ms = remaining_ms(&end);
        if (ms < 0) { perror("clock_gettime"); return -1; }
        if (!ms) { fprintf(stderr, "stdin R handshake timed out\n"); return -1; }
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN | POLLHUP };
        int pr = poll(&pfd, 1, ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) { perror("poll stdin"); return -1; }
        if (!pr) continue;
        unsigned char c; ssize_t got;
        do { got = read(STDIN_FILENO, &c, 1); } while (got < 0 && errno == EINTR);
        if (got != 1 || c != 'R') { fprintf(stderr, "expected one R byte on stdin\n"); return -1; }
        break;
    }
    for (unsigned page = 0; page < MAP_BYTES / PAGE_BYTES; ++page) {
        unsigned char expected[PAGE_BYTES]; page_data(expected, page, nonce);
        const unsigned char *actual = mapping + (size_t)page * PAGE_BYTES;
        if (memcmp(actual, expected, PAGE_BYTES)) { fprintf(stderr, "readback mismatch at page %u\n", page); return -1; }
    }
    puts("PAGEOUT_READBACK_PASS"); fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        uintmax_t n;
        assert(!decimal("67108864", &n) && n == 67108864);
        assert(decimal("", &n) && decimal("1x", &n) && decimal("-1", &n));
        assert(pagemap_entry(SWAPPED | 7, NULL) == 1);
        uint64_t off = 0; assert(pagemap_entry(SWAPPED | (UINT64_C(1234) << 5), &off) == 1 && off == 1234);
        assert(!pagemap_entry(PRESENT | SWAPPED | 3, NULL));
        assert(!pagemap_entry(PRESENT, NULL) && !pagemap_entry(0, NULL));
        unsigned char a[PAGE_BYTES], b[PAGE_BYTES]; page_data(a, 0, 7); page_data(b, 0, 7);
        assert(!memcmp(a, b, sizeof(a)));
        page_data(b, 0, 8); assert(memcmp(a, b, sizeof(a)));
        assert(cgroup_membership_matches("0::/vram-pageout-test-123\n", 123));
        assert(cgroup_membership_matches("0::/vram-pageout-test-123", 123));
        assert(!cgroup_membership_matches("0::/other\n", 123));
        assert(!cgroup_membership_matches("0::/vram-pageout-test-123/child\n", 123));
        assert(!cgroup_membership_matches("0::/vram-pageout-test-123\n1:cpu:/x\n", 123));
        assert(limit_matches("67108864", "67108864") && !limit_matches("67108865", "67108864"));
        assert(limit_matches("33554432", "33554432") && !limit_matches("33554431", "33554432"));
        assert(limit_matches("0", "0") && !limit_matches("1", "0"));
        assert(limit_matches("1", "1") && !limit_matches("0", "1"));
        puts("PASS --self-test"); return 0;
    }
    uintmax_t target_value;
    int check_device_only = argc == 6 && !strcmp(argv[5], "--check-device-only");
    if ((!check_device_only && argc != 5) || (check_device_only && argc != 6) ||
        strcmp(argv[1], "--device") || strcmp(argv[2], "/dev/ublkb31") ||
        strcmp(argv[3], "--target-pid") || decimal(argv[4], &target_value) || !target_value || target_value > INT_MAX) {
        fprintf(stderr, "usage: %s --device /dev/ublkb31 --target-pid DECIMAL_PID [--check-device-only]\n", argv[0]); return 2;
    }
    pid_t target_pid = (pid_t)target_value;
    if ((uintmax_t)target_pid != target_value) { fprintf(stderr, "target PID out of range\n"); return 2; }
    if (geteuid() != 0) { fprintf(stderr, "admission: root required\n"); return 1; }
    if (!check_device_only && admit_cgroup()) return 1;
    struct ublksrv_dev_data data = { .dev_id = UBLK_ID };
    struct ublksrv_ctrl_dev *ctrl = ublksrv_ctrl_init(&data);
    if (!ctrl) { fprintf(stderr, "ublksrv_ctrl_init failed\n"); return 1; }
    int devfd = -1, rc = 1;
    unsigned char *mapping = MAP_FAILED;
    struct stat st; uint64_t capacity = 0; char sys_path[128], resolved[4096];
    if (valid_info(ctrl, target_pid)) goto cleanup;
    devfd = open(argv[2], O_RDONLY | O_DIRECT | O_CLOEXEC | O_NOFOLLOW);
    if (devfd < 0) { perror("open ublk device"); goto cleanup; }
    if (fstat(devfd, &st) || !S_ISBLK(st.st_mode) || ioctl(devfd, BLKGETSIZE64, &capacity) < 0 || capacity != DEVICE_BYTES) {
        fprintf(stderr, "block device type or 128 MiB capacity check failed\n"); goto cleanup;
    }
    snprintf(sys_path, sizeof(sys_path), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(sys_path, resolved)) { perror("realpath sysfs block device"); goto cleanup; }
    const char *base = strrchr(resolved, '/');
    if (!base || strcmp(base + 1, "ublkb31")) { fprintf(stderr, "sysfs block-device name mismatch\n"); goto cleanup; }
    if (valid_info(ctrl, target_pid)) goto cleanup;
    if (check_device_only) {
        puts("DEVICE_ADMISSION_PASS"); fflush(stdout); rc = 0; goto cleanup;
    }
    uint64_t nonce;
    ssize_t random_n;
    do { random_n = getrandom(&nonce, sizeof(nonce), 0); } while (random_n < 0 && errno == EINTR);
    if (random_n != (ssize_t)sizeof(nonce)) { perror("getrandom nonce"); goto cleanup; }
    mapping = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) { perror("mmap"); goto cleanup; }
    if (madvise(mapping, MAP_BYTES, MADV_NOHUGEPAGE)) { perror("MADV_NOHUGEPAGE"); goto cleanup; }
    for (unsigned page = 0; page < MAP_BYTES / PAGE_BYTES; ++page) page_data(mapping + (size_t)page * PAGE_BYTES, page, nonce);
    if (madvise(mapping, MAP_BYTES, MADV_PAGEOUT)) { perror("MADV_PAGEOUT"); goto cleanup; }
    if (!wait_ready(mapping, devfd, nonce)) rc = 0;
cleanup:
    if (mapping != MAP_FAILED && munmap(mapping, MAP_BYTES)) { perror("munmap"); rc = 1; }
    if (devfd >= 0 && close(devfd)) { perror("close block device"); rc = 1; }
    ublksrv_ctrl_deinit(ctrl);
    return rc;
}
