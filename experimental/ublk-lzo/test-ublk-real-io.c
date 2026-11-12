/* Guest-only, destructive fixture for an already-created disposable device.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -pthread test-ublk-real-io.c -o /tmp/test-ublk-real-io
 * Run: /tmp/test-ublk-real-io DEVICE CAPACITY_BYTES
 * Checks only the first six pages and the last page for initial zeroes;
 * these pages are overwritten. Capacity must equal the device/file size.
 * Uses O_DIRECT; unaligned rejection is mandatory for block devices and may
 * be unavailable on regular files. Does not test flush, FUA, discard,
 * write-zeroes or durability.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PAGE = 4096, WORKERS = 4, ROUNDS = 32 };
struct worker { int fd; off_t offset; unsigned id; int failed; };

static int io_page(int fd, void *buf, off_t offset, int write_op)
{
    ssize_t n;
    do {
        n = write_op ? pwrite(fd, buf, PAGE, offset) : pread(fd, buf, PAGE, offset);
    } while (n < 0 && errno == EINTR);
    if (n != PAGE) {
        fprintf(stderr, "%s offset=%jd: %s (returned %zd)\n",
                write_op ? "write" : "read", (intmax_t)offset,
                n < 0 ? strerror(errno) : "short I/O", n);
        return -1;
    }
    return 0;
}

static void pattern(unsigned char *buf, unsigned seed, int random_data)
{
    uint32_t x = seed | 1U;
    for (int i = 0; i < PAGE; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = random_data ? (unsigned char)x : (unsigned char)(seed + i % 32);
    }
}

static int cycle(int fd, off_t offset, unsigned seed, int random_data,
                 unsigned char *in, unsigned char *out)
{
    pattern(in, seed, random_data);
    if (io_page(fd, in, offset, 1) || io_page(fd, out, offset, 0)) return -1;
    if (memcmp(in, out, PAGE)) {
        fprintf(stderr, "data mismatch at offset=%jd\n", (intmax_t)offset);
        return -1;
    }
    return 0;
}

static void *run_worker(void *arg)
{
    struct worker *w = arg;
    unsigned char *in = NULL, *out = NULL;
    if (posix_memalign((void **)&in, PAGE, PAGE) ||
        posix_memalign((void **)&out, PAGE, PAGE)) {
        fprintf(stderr, "worker allocation failed\n");
        w->failed = 1;
    } else {
        for (unsigned i = 0; i < ROUNDS; i++) {
            if (cycle(w->fd, w->offset, 1000 + w->id * ROUNDS + i, i & 1, in, out)) {
                w->failed = 1;
                break;
            }
        }
    }
    free(in); free(out);
    return NULL;
}

int main(int argc, char **argv)
{
    int verify_only = argc == 4 && !strcmp(argv[3], "--verify-final-random");
    int digest_only = argc == 4 && !strcmp(argv[3], "--digest-only");
    if ((argc != 3 && !verify_only && !digest_only) || !*argv[2] || strspn(argv[2], "0123456789") != strlen(argv[2])) {
        fprintf(stderr, "usage: %s DISPOSABLE_DEVICE CAPACITY_BYTES\n", argv[0]);
        return 2;
    }
    errno = 0;
    char *end;
    uintmax_t capacity = strtoumax(argv[2], &end, 10);
    if (errno || *end || capacity < 7 * PAGE || capacity % PAGE || capacity > INT64_MAX) {
        fprintf(stderr, "capacity must be a multiple of 4096, at least 28672 bytes, and fit in off_t\n");
        return 2;
    }
    int fd = open(argv[1], (digest_only ? O_RDONLY : O_RDWR) | O_DIRECT | O_CLOEXEC);
    if (fd < 0) { perror("open O_DIRECT"); return 1; }
    struct stat st;
    uint64_t actual = 0;
    if (fstat(fd, &st) ||
        (S_ISBLK(st.st_mode) ? ioctl(fd, BLKGETSIZE64, &actual) :
         S_ISREG(st.st_mode) ? (actual = (uint64_t)st.st_size, 0) : -1)) {
        fprintf(stderr, "cannot determine regular file or block device capacity\n");
        close(fd); return 1;
    }
    if (actual != capacity) {
        fprintf(stderr, "capacity mismatch: actual=%" PRIu64 " requested=%ju\n", actual, capacity);
        close(fd); return 1;
    }
    unsigned char *in = NULL, *out = NULL;
    int rc = 1;
    if (posix_memalign((void **)&in, PAGE, PAGE) || posix_memalign((void **)&out, PAGE, PAGE)) {
        fprintf(stderr, "allocation failed\n"); goto done;
    }
    if (digest_only) {
        uint64_t digest = UINT64_C(14695981039346656037);
        for (uintmax_t offset = 0; offset < capacity; offset += PAGE) {
            if (io_page(fd, out, (off_t)offset, 0)) goto done;
            for (size_t i = 0; i < PAGE; ++i) {
                digest ^= out[i];
                digest *= UINT64_C(1099511628211);
            }
        }
        printf("UBLK_DEVICE_DIGEST bytes=%ju fnv1a64=%016" PRIx64 "\n", capacity, digest);
        rc = 0; goto done;
    }
    if (verify_only) goto verify_full_device;
    memset(in, 0, PAGE);
    for (int i = 0; i < 7; i++) {
        off_t offset = i == 6 ? (off_t)capacity - PAGE : (off_t)i * PAGE;
        if (io_page(fd, out, offset, 0) || memcmp(in, out, PAGE)) {
            fprintf(stderr, "initial page is not zero at offset=%jd\n", (intmax_t)offset);
            goto done;
        }
    }
    if (cycle(fd, 0, 42, 0, in, out) || cycle(fd, PAGE, 43, 1, in, out) ||
        cycle(fd, 0, 44, 1, in, out) || cycle(fd, PAGE, 45, 0, in, out) ||
        cycle(fd, (off_t)capacity - PAGE, 46, 1, in, out)) goto done;

    errno = 0;
    ssize_t n;
    do { n = pwrite(fd, in, PAGE, 1); } while (n < 0 && errno == EINTR);
    if (S_ISREG(st.st_mode) && n == PAGE) {
        puts("SKIP unaligned rejection: regular filesystem accepted O_DIRECT I/O");
    } else if (n >= 0 || errno != EINVAL) {
        fprintf(stderr, "unaligned direct write was not rejected with EINVAL (returned %zd, errno=%d)\n", n, errno);
        goto done;
    }
    do { n = pread(fd, out, PAGE, (off_t)capacity); } while (n < 0 && errno == EINTR);
    if (n != 0) {
        fprintf(stderr, "read at end did not return EOF (returned %zd)\n", n);
        goto done;
    }
    struct worker workers[WORKERS];
    pthread_t threads[WORKERS];
    int started = 0, failed = 0;
    for (int i = 0; i < WORKERS; i++) {
        workers[i] = (struct worker){ .fd = fd, .offset = (off_t)(i + 2) * PAGE, .id = (unsigned)i };
        int err = pthread_create(&threads[i], NULL, run_worker, &workers[i]);
        if (err) { fprintf(stderr, "pthread_create: %s\n", strerror(err)); failed = 1; break; }
        started++;
    }
    for (int i = 0; i < started; i++) {
        int err = pthread_join(threads[i], NULL);
        if (err) { fprintf(stderr, "pthread_join: %s\n", strerror(err)); failed = 1; }
        else if (workers[i].failed) failed = 1;
    }
    if (failed) goto done;
    for (int i = 0; i < WORKERS; i++) {
        pattern(in, 1000 + (unsigned)i * ROUNDS + ROUNDS - 1, 1);
        if (io_page(fd, out, (off_t)(i + 2) * PAGE, 0) || memcmp(in, out, PAGE)) {
            fprintf(stderr, "concurrent final verification failed for worker %d\n", i); goto done;
        }
    }
    for (uintmax_t page = 0; page < capacity / PAGE; page++) {
        pattern(in, (unsigned)page + 2001, 1);
        if (io_page(fd, in, (off_t)(page * PAGE), 1)) goto done;
    }
verify_full_device:
    for (uintmax_t page = 0; page < capacity / PAGE; page++) {
        pattern(in, (unsigned)page + 2001, 1);
        if (io_page(fd, out, (off_t)(page * PAGE), 0) || memcmp(in, out, PAGE)) {
            fprintf(stderr, "full-device verification failed at page=%ju\n", page);
            goto done;
        }
    }
    puts(verify_only ? "PASS retained full-device random data" : "PASS zero pages, aligned I/O, overwrites, boundaries, disjoint concurrency and full-device random data");
    rc = 0;
done:
    free(in); free(out);
    if (close(fd)) { perror("close"); rc = 1; }
    return rc;
}
