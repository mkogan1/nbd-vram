/* Guest-only anonymous-memory swap integrity check.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror test-ublk-swap-pressure.c -o /tmp/test-ublk-swap-pressure
 * Run ONLY in the isolated guest: /tmp/test-ublk-swap-pressure MiB [pressure-MiB].
 * Data: 1..512 MiB; optional additional pressure: 1..384 MiB (default none).
 * MADV_PAGEOUT is advisory; positive device-wide Used does not prove that
 * every page (or any particular page) of this mapping reached /dev/ublkb0.
 * Existing swap usage, other swap devices and zswap can affect that evidence.
 * One fill/pageout/read cycle checks neither sustained load nor durability.
 * The mapping is bounded, but guest OOM and kernel/I/O stalls remain possible.
 * VRAM_PAGEOUT_BARRIER=1 pauses after reclaim for /tmp/pageout-resume (10s).
 * The parent creates/removes resume; the helper creates/removes pageout-ready.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ublk-guest-guard.h"

static int parse_mib(const char *arg, size_t *bytes, uintmax_t maximum)
{
    char *end;
    uintmax_t mib;
    if (!*arg || strspn(arg, "0123456789") != strlen(arg))
        return -1;
    errno = 0;
    mib = strtoumax(arg, &end, 10);
    if (errno || *end || mib < 1 || mib > maximum)
        return -1;
    *bytes = (size_t)mib * 1024 * 1024;
    return 0;
}

static unsigned char next_byte(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return (unsigned char)*state;
}

/* Return 1 for positive Used on the exact device, 0 if absent/unused. */
static int swap_used(uintmax_t *used)
{
    FILE *f = fopen("/proc/swaps", "r");
    char line[4096], device[4096], type[32];
    uintmax_t size, value;
    int priority, found = 0;
    if (!f) {
        perror("open /proc/swaps");
        return -1;
    }
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "cannot read /proc/swaps header\n");
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') ||
            sscanf(line, "%4095s %31s %ju %ju %d", device, type,
                   &size, &value, &priority) != 5) {
            fprintf(stderr, "malformed /proc/swaps row\n");
            found = -1;
            break;
        }
        if (!strcmp(device, "/dev/ublkb0") && value > 0) {
            *used = value;
            found = 1;
        }
    }
    if (ferror(f)) {
        fprintf(stderr, "read /proc/swaps failed\n");
        found = -1;
    }
    if (fclose(f)) {
        perror("close /proc/swaps");
        found = -1;
    }
    return found;
}

static int pageout_barrier(void)
{
    const char *enabled = getenv("VRAM_PAGEOUT_BARRIER");
    const char *ready = "/tmp/pageout-ready", *resume = "/tmp/pageout-resume";
    struct stat created, current;
    struct timespec deadline, now;
    const int timeout_seconds = 120;
    int rc = -1;
    if (!enabled || strcmp(enabled, "1"))
        return 0;
    if (lstat(resume, &current) == 0 || errno != ENOENT) {
        fprintf(stderr, "FAIL: pageout resume marker already exists or cannot be checked\n");
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
        perror("pageout barrier clock");
        return -1;
    }
    deadline.tv_sec += timeout_seconds;
    int fd = open(ready, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("create pageout-ready");
        return -1;
    }
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            perror("pageout barrier clock");
            break;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            fprintf(stderr, "FAIL: pageout barrier timed out after %ds\n", timeout_seconds);
            break;
        }
        if (lstat(resume, &current) == 0) {
            if (!S_ISREG(current.st_mode) || current.st_uid != geteuid()) {
                fprintf(stderr, "FAIL: invalid pageout resume marker\n");
                break;
            }
            rc = 0;
            break;
        }
        if (errno != ENOENT) {
            perror("check pageout-resume");
            break;
        }
        struct timespec wake = now;
        wake.tv_nsec += 100000000;
        if (wake.tv_nsec >= 1000000000) {
            ++wake.tv_sec;
            wake.tv_nsec -= 1000000000;
        }
        if (wake.tv_sec > deadline.tv_sec ||
            (wake.tv_sec == deadline.tv_sec && wake.tv_nsec > deadline.tv_nsec))
            wake = deadline;
        int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        if (error && error != EINTR) {
            errno = error;
            perror("pageout barrier wait");
            break;
        }
    }
    /* Only remove our ready inode; resume belongs to the parent. */
    if (fstat(fd, &created) || lstat(ready, &current)) {
        perror("stat pageout-ready for cleanup");
        rc = -1;
    } else if (created.st_dev != current.st_dev || created.st_ino != current.st_ino) {
        fprintf(stderr, "FAIL: pageout-ready replaced; leaving it untouched\n");
        rc = -1;
    } else if (unlink(ready)) {
        perror("unlink pageout-ready");
        rc = -1;
    }
    if (close(fd)) {
        perror("close pageout-ready");
        rc = -1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    size_t bytes, pressure_bytes = 0;
    volatile unsigned char *pressure = MAP_FAILED;
    int rc = 1, observed = 0;
    uintmax_t used = 0;
    uint32_t state = UINT32_C(0x12345678);
    if ((argc != 2 && argc != 3) || parse_mib(argv[1], &bytes, 512) ||
        (argc == 3 && parse_mib(argv[2], &pressure_bytes, 384))) {
        fprintf(stderr, "usage: %s MiB (decimal integer 1..512) [pressure-MiB (decimal integer 1..384)]\n", argv[0]);
        return 2;
    }
    if (ublk_guest_guard()) {
        fprintf(stderr, "ublk guest guard rejected environment; no allocation\n");
        return 1;
    }
    unsigned char *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (size_t i = 0; i < bytes; ++i)
        data[i] = next_byte(&state);
    if (madvise(data, bytes, MADV_PAGEOUT)) {
        perror("madvise MADV_PAGEOUT");
        goto done;
    }
    /* Do not access the mapping until device swap usage has been observed. */
    for (unsigned attempt = 0; attempt <= 50; ++attempt) {
        observed = swap_used(&used);
        if (observed < 0)
            goto done;
        if (observed || attempt == 50)
            break;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };
        while (nanosleep(&delay, &delay)) {
            if (errno != EINTR) {
                perror("nanosleep");
                goto done;
            }
        }
    }
    if (!observed) {
        fprintf(stderr, "FAIL: /dev/ublkb0 has no positive Used after pageout (5s polling budget)\n");
        goto done;
    }
    /* Allow writeback, then ask reclaim to remove clean swapcache pages. */
    struct timespec settle = { .tv_sec = 2, .tv_nsec = 0 };
    while (nanosleep(&settle, &settle)) {
        if (errno != EINTR) { perror("writeback wait"); goto done; }
    }
    if (madvise(data, bytes, MADV_PAGEOUT)) {
        perror("second MADV_PAGEOUT"); goto done;
    }
    if (pressure_bytes) {
        pressure = mmap(NULL, pressure_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pressure == MAP_FAILED) {
            perror("pressure mmap");
            goto done;
        }
        /* Fault in every byte; volatile prevents removal of these pressure stores. */
        for (size_t i = 0; i < pressure_bytes; ++i)
            pressure[i] = next_byte(&state);
        if (madvise(data, bytes, MADV_PAGEOUT)) {
            perror("pressure MADV_PAGEOUT"); goto done;
        }
        FILE *meminfo = fopen("/proc/meminfo", "r");
        if (!meminfo) { perror("meminfo"); goto done; }
        char line[256];
        while (fgets(line, sizeof(line), meminfo))
            if (!strncmp(line, "MemAvailable:", 13) || !strncmp(line, "SwapCached:", 11))
                fputs(line, stdout);
        fclose(meminfo);
    }
    FILE *reclaim_file = fopen("/sys/fs/cgroup/pageout/memory.reclaim", "w");
    if (reclaim_file) {
        int failed = fprintf(reclaim_file, "%zu swappiness=max\n", bytes) < 0;
        if (fclose(reclaim_file) != 0) failed = 1;
        if (failed) fprintf(stderr, "memory.reclaim did not fully satisfy request\n");
    }
    int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) { perror("pagemap"); goto done; }
    size_t swapped = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) { close(pagemap); goto done; }
    for (size_t offset = 0; offset < bytes; offset += (size_t)page_size) {
        uint64_t entry;
        off_t pos = (off_t)(((uintptr_t)data + offset) / (size_t)page_size * sizeof(entry));
        ssize_t n;
        do { n = pread(pagemap, &entry, sizeof(entry), pos); } while (n < 0 && errno == EINTR);
        if (n != sizeof(entry)) { close(pagemap); goto done; }
        swapped += !!(entry & (UINT64_C(1) << 62));
    }
    close(pagemap);
    if (!swapped) { fprintf(stderr, "FAIL: mapping has no swapped pages\n"); goto done; }
    printf("MAPPING_SWAPPED_PAGES=%zu\n", swapped);
    if (pageout_barrier())
        goto done;
    state = UINT32_C(0x12345678);
    size_t mismatches = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char expected = next_byte(&state);
        if (data[i] != expected) {
            if (!mismatches)
                fprintf(stderr, "first mismatch at byte %zu: got %u expected %u\n",
                        i, (unsigned)data[i], (unsigned)expected);
            ++mismatches;
        }
    }
    if (mismatches) {
        fprintf(stderr, "FAIL: %zu of %zu bytes differ\n", mismatches, bytes);
        goto done;
    }
    printf("PASS: verified all %zu bytes; /dev/ublkb0 Used=%ju KiB before readback\n",
           bytes, used);
    rc = 0;
done:
    if (pressure != MAP_FAILED && munmap((void *)pressure, pressure_bytes)) {
        perror("pressure munmap");
        rc = 1;
    }
    if (munmap(data, bytes)) {
        perror("munmap");
        rc = 1;
    }
    return rc;
}
