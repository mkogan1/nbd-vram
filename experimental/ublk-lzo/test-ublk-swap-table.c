/* Mock-only inclusion test of the actual swap_table_empty; no target main runs.
 * Build with real headers, without libublksrv:
 * cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections \
 *   -fdata-sections -isystem /var/tmp/ublk-compare-src/include \
 *   -isystem /var/tmp/ublk-compare-src test-ublk-swap-table.c \
 *   -Wl,--gc-sections -pthread -ldl -o /var/tmp/test-ublk-swap-table
 * /var/tmp/test-ublk-swap-table
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static FILE *fixture_fopen(const char *path, const char *mode);
static int fixture_fclose(FILE *stream);
int main(int, char **) __asm__("unused_ublk_target_main");
#define fopen fixture_fopen
#define fclose fixture_fclose
#include "ublk-lzo-target.c"
#undef fopen
#undef fclose

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s line %d: %s\n", case_name, __LINE__, #expr); \
    exit(1); \
} } while (0)

#define HEADER "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
static const char *case_name, *contents;
static FILE *opened;
static size_t position;
static int open_failure, read_failure, close_failure;
static int opens, closes, read_errors, close_errors;

/* A real stdio error after the supplied bytes, including after a full header. */
static ssize_t failing_read(void *cookie, char *buf, size_t size)
{
    size_t remaining = strlen(contents) - position;
    (void)cookie;
    if (!remaining) {
        ++read_errors;
        errno = EIO;
        return -1;
    }
    if (size > remaining) size = remaining;
    memcpy(buf, contents + position, size);
    position += size;
    return (ssize_t)size;
}

static FILE *fixture_fopen(const char *path, const char *mode)
{
    CHECK(!strcmp(path, "/proc/swaps") && !strcmp(mode, "r"));
    CHECK(++opens == 1);
    if (open_failure) { errno = EACCES; return NULL; }
    if (read_failure) {
        cookie_io_functions_t io = { .read = failing_read };
        opened = fopencookie(NULL, "r", io);
    } else {
        opened = fmemopen((void *)contents, strlen(contents), "r");
    }
    CHECK(opened != NULL);
    return opened;
}

static int fixture_fclose(FILE *stream)
{
    CHECK(stream == opened && ++closes == 1);
    CHECK(fclose(stream) == 0);
    opened = NULL;
    if (close_failure) { ++close_errors; errno = EIO; return EOF; }
    return 0;
}

int swap_table_test_main(void) __asm__("main");
int swap_table_test_main(void)
{
    static const struct {
        const char *name, *text;
        int expected, fail_open, fail_read, fail_close;
    } cases[] = {
        { "exact-empty-header", HEADER, 1, 0, 0, 0 },
        { "used-zero-device", HEADER "/dev/ublkb0\tpartition\t1024\t0\t-2\n", 0, 0, 0, 0 },
        { "used-positive-device", HEADER "/dev/ublkb0\tpartition\t1024\t512\t-2\n", 0, 0, 0, 0 },
        { "used-zero-alias", HEADER "/dev/disk/by-uuid/alias\tpartition\t1024\t0\t-2\n", 0, 0, 0, 0 },
        { "used-positive-alias", HEADER "/dev/mapper/alias\tpartition\t1024\t1\t-2\n", 0, 0, 0, 0 },
        { "used-zero-file", HEADER "/private/swapfile\tfile\t1024\t0\t-2\n", 0, 0, 0, 0 },
        { "empty-file", "", 0, 0, 0, 0 },
        { "truncated-header", "Filename\t\t\t\tType", 0, 0, 0, 0 },
        { "missing-newline", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority", 0, 0, 0, 0 },
        { "spaces-in-header", "Filename Type Size Used Priority\n", 0, 0, 0, 0 },
        { "crlf-header", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\r\n", 0, 0, 0, 0 },
        { "wrong-column", "Filename\t\t\t\tType\t\tSize\t\tFree\t\tPriority\n", 0, 0, 0, 0 },
        { "extra-blank-line", HEADER "\n", 0, 0, 0, 0 },
        { "malformed-row", HEADER "garbage\n", 0, 0, 0, 0 },
        { "truncated-row", HEADER "/dev/alias", 0, 0, 0, 0 },
        { "open-error", HEADER, 0, 1, 0, 0 },
        { "header-read-error", "", 0, 0, 1, 0 },
        { "partial-header-read-error", "Filename\t", 0, 0, 1, 0 },
        { "post-header-read-error", HEADER, 0, 0, 1, 0 },
        { "empty-close-error", HEADER, 0, 0, 0, 1 },
        { "malformed-close-error", "garbage\n", 0, 0, 0, 1 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        case_name = cases[i].name;
        contents = cases[i].text;
        open_failure = cases[i].fail_open;
        read_failure = cases[i].fail_read;
        close_failure = cases[i].fail_close;
        position = 0;
        opens = closes = read_errors = close_errors = 0;
        CHECK(swap_table_empty() == cases[i].expected);
        CHECK(opens == 1 && closes == !open_failure && opened == NULL);
        CHECK((read_errors > 0) == read_failure);
        CHECK(close_errors == close_failure);
        printf("PASS %s\n", case_name);
    }
    puts("MOCK_ONLY_PASS real_swap_table_empty=1 full_target_main=0");
    return 0;
}
