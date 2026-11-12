/* CPU-only tests for the real admission helper; no device or CUDA calls. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fixture_euid;
static const char *fixture_text;
static int fixture_read_failure, fixture_close_failure;
static size_t fixture_pos;
static FILE *fixture_stream;
static char *fixture_env_items[8];
static char **fixture_env = fixture_env_items;
static int fixture_env_count;

static uid_t fixture_geteuid(void) { return (uid_t)fixture_euid; }
static FILE *fixture_fopen(const char *path, const char *mode);
static int fixture_fclose(FILE *stream);
static ssize_t fixture_read(void *cookie, char *buf, size_t size);

#define geteuid fixture_geteuid
#define fopen fixture_fopen
#define fclose fixture_fclose
#define environ fixture_env
#include "ublk-host-admission.h"
#undef environ
#undef fclose
#undef fopen
#undef geteuid

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static ssize_t fixture_read(void *cookie, char *buf, size_t size)
{
    size_t remaining = strlen(fixture_text) - fixture_pos;
    (void)cookie;
    if (!remaining) {
        if (fixture_read_failure) { errno = EIO; return -1; }
        return 0;
    }
    if (size > remaining) size = remaining;
    memcpy(buf, fixture_text + fixture_pos, size);
    fixture_pos += size;
    return (ssize_t)size;
}

static FILE *fixture_fopen(const char *path, const char *mode)
{
    cookie_io_functions_t io = { .read = fixture_read };
    CHECK(!strcmp(path, "/proc/meminfo") && !strcmp(mode, "r"));
    fixture_pos = 0;
    fixture_stream = fopencookie(NULL, "r", io);
    return fixture_stream;
}

static int fixture_fclose(FILE *stream)
{
    CHECK(stream == fixture_stream);
    fixture_stream = NULL;
    (void)fclose(stream);
    if (fixture_close_failure) { errno = EIO; return EOF; }
    return 0;
}

static void setup(const char *text)
{
    fixture_euid = 0;
    fixture_text = text;
    fixture_read_failure = fixture_close_failure = 0;
    fixture_env_count = 0;
    fixture_env_items[0] = NULL;
}

static void env(const char *value)
{
    fixture_env_items[fixture_env_count++] = (char *)value;
    fixture_env_items[fixture_env_count] = NULL;
}

int main(void)
{
    setup("MemAvailable: 1048576 kB\n");
    CHECK(ublk_host_admit(1, 1, 0));
    CHECK(!ublk_host_admit(0, 1, 0));
    CHECK(!ublk_host_admit(1, 0, 0));
    fixture_euid = 1; CHECK(!ublk_host_admit(1, 1, 0));

    for (size_t i = 0; i < 4; ++i) {
        static const char *const bad[] = {
            "VRAM_TEST_X=", "CUDA_FIXTURE_X=", "CUDA_BRIDGE_DEVICE=", "LD_PRELOAD="
        };
        setup("MemAvailable: 1048576 kB\n"); env(bad[i]);
        CHECK(!ublk_host_admit(1, 1, 0));
    }

    setup("MemAvailable: 1048577 kB\n");
    CHECK(ublk_host_admit(1, 1, 512));
    CHECK(!ublk_host_admit(1, 1, 513));
    setup("MemAvailable: 1048576 kB\n");
    CHECK(ublk_host_admit(1, 1, 0));

    static const char *const malformed[] = {
        "", "MemAvailable: 1 kB\nMemAvailable: 2 kB\n",
        "MemAvailable: 1 MB\n", "MemAvailable: 1 kB extra\n",
        "MemAvailable: 18446744073709551616 kB\n",
        "MemAvailable: 18014398509481984 kB\n"
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        setup(malformed[i]); CHECK(!ublk_host_admit(1, 1, 0));
    }
    setup("MemAvailable: 1048576 kB\n");
    fixture_read_failure = 1; CHECK(!ublk_host_admit(1, 1, 0));
    setup("MemAvailable: 1048576 kB\n");
    fixture_close_failure = 1; CHECK(!ublk_host_admit(1, 1, 0));
    puts("PASS ublk_host_admit");
    return 0;
}
