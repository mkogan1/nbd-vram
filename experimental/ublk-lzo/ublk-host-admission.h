#ifndef UBLK_HOST_ADMISSION_H
#define UBLK_HOST_ADMISSION_H

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* Returns 1 only when the startup requirements and RAM budget are satisfied. */
static int ublk_host_admit(int cuda, int safe_stop, uint64_t metadata_bytes)
{
    const uint64_t reserve = UINT64_C(1024) * 1024 * 1024;
    char **entry;
    FILE *meminfo;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    uint64_t available_kb = 0, available;
    unsigned matches = 0;
    int valid = 1, close_result;

    if (geteuid() != 0 || !cuda || !safe_stop)
        return 0;
    for (entry = environ; entry && *entry; ++entry)
        if (!strncmp(*entry, "VRAM_TEST_", 10) ||
            !strncmp(*entry, "CUDA_FIXTURE_", 13) ||
            !strncmp(*entry, "CUDA_BRIDGE_DEVICE=", 19) ||
            !strncmp(*entry, "LD_PRELOAD=", 11))
            return 0;

    meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo)
        return 0;
    errno = 0;
    while ((length = getline(&line, &capacity, meminfo)) >= 0) {
        static const char key[] = "MemAvailable:";
        char *p;
        uint64_t value = 0;
        if ((size_t)length < sizeof(key) - 1 ||
            memcmp(line, key, sizeof(key) - 1))
            continue;
        ++matches;
        p = line + sizeof(key) - 1;
        if (!isspace((unsigned char)*p)) { valid = 0; continue; }
        while (isspace((unsigned char)*p)) ++p;
        if (!isdigit((unsigned char)*p)) { valid = 0; continue; }
        do {
            unsigned digit = (unsigned)(*p++ - '0');
            if (value > (UINT64_MAX - digit) / 10) { valid = 0; break; }
            value = value * 10 + digit;
        } while (isdigit((unsigned char)*p));
        if (!valid) continue;
        while (isspace((unsigned char)*p)) ++p;
        if (strncmp(p, "kB", 2)) { valid = 0; continue; }
        p += 2;
        while (isspace((unsigned char)*p)) ++p;
        if (*p) { valid = 0; continue; }
        available_kb = value;
    }
    if (ferror(meminfo) || !feof(meminfo)) valid = 0;
    close_result = fclose(meminfo);
    free(line);
    if (close_result != 0 || !valid || matches != 1 ||
        available_kb > UINT64_MAX / 1024)
        return 0;
    available = available_kb * 1024;
    return available >= reserve && metadata_bytes <= (available - reserve) / 2;
}

#endif
