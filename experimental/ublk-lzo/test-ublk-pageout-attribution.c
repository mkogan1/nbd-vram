#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define main pageout_program_main
#define open fixture_open
#define pread fixture_pread
#define close fixture_close
#define sysconf fixture_sysconf
#include "test-ublk-host-pageout.c"
#undef sysconf
#undef close
#undef pread
#undef open
#undef main

#define FIXTURE_PAGES (MAP_BYTES / PAGE_BYTES)
#define FIXTURE_TYPE 5U
#define FIXTURE_NONCE UINT64_C(0x7a31d9e4b62c805f)

static uint64_t entries[FIXTURE_PAGES];
static unsigned char device[DEVICE_BYTES];
static size_t shortread_page = SIZE_MAX;
static size_t mapping_page_base;
static int fixture_pagemap_fd = 41;
static int fixture_device_fd = 42;

int fixture_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/proc/self/pagemap"));
    return fixture_pagemap_fd;
}

ssize_t fixture_pread(int fd, void *buf, size_t count, off_t offset)
{
    assert(offset >= 0);
    if (fd == fixture_pagemap_fd) {
        assert(count == sizeof(uint64_t));
        uint64_t index = (uint64_t)offset / sizeof(uint64_t);
        assert((uint64_t)offset % sizeof(uint64_t) == 0);
        assert(index >= mapping_page_base && index - mapping_page_base < FIXTURE_PAGES);
        size_t page = (size_t)(index - mapping_page_base);
        if (page == shortread_page) return (ssize_t)count - 1;
        memcpy(buf, &entries[page], count);
        return (ssize_t)count;
    }
    assert(fd == fixture_device_fd);
    if ((uint64_t)offset >= sizeof(device) || count > sizeof(device) - (size_t)offset) return 0;
    memcpy(buf, device + (size_t)offset, count);
    return (ssize_t)count;
}

int fixture_close(int fd)
{
    assert(fd == fixture_pagemap_fd);
    return 0;
}

long fixture_sysconf(int name)
{
    assert(name == _SC_PAGESIZE);
    return PAGE_BYTES;
}

static uint64_t swap_entry(unsigned type, uint64_t offset)
{
    return SWAPPED | (offset << 5) | type;
}

static void set_page(unsigned page, unsigned type, uint64_t offset, int matching)
{
    entries[page] = swap_entry(type, offset);
    if (offset >= DEVICE_BYTES / PAGE_BYTES) return;
    if (matching) {
        unsigned char data[PAGE_BYTES];
        page_data(data, page, FIXTURE_NONCE);
        memcpy(device + offset * PAGE_BYTES, data, sizeof(data));
    } else {
        memset(device + offset * PAGE_BYTES, 0xa5, PAGE_BYTES);
    }
}

static int run_case(size_t *count)
{
    unsigned char *mapping = (unsigned char *)(uintptr_t)((uint64_t)mapping_page_base * PAGE_BYTES);
    *count = SIZE_MAX;
    return inspect_swapped(mapping, count, fixture_device_fd, FIXTURE_NONCE);
}

static void reset_fixture(void)
{
    memset(entries, 0, sizeof(entries));
    memset(device, 0, sizeof(device));
    shortread_page = SIZE_MAX;
}

int main(void)
{
    mapping_page_base = 32;
    size_t count;

    reset_fixture();
    set_page(0, FIXTURE_TYPE, 3, 1);
    set_page(1, FIXTURE_TYPE, 4, 1);
    set_page(2, 0, 5, 0);
    set_page(3, 0, DEVICE_BYTES / PAGE_BYTES, 1);
    set_page(4, 0, 6, 0);
    int rc = run_case(&count);
    assert(rc == 0 && count == 2);

    reset_fixture();
    set_page(0, FIXTURE_TYPE, 3, 1);
    set_page(1, FIXTURE_TYPE, 4, 0);
    assert(run_case(&count) == -1 && count == SIZE_MAX);

    reset_fixture();
    set_page(0, FIXTURE_TYPE, 3, 0);
    set_page(1, FIXTURE_TYPE, 4, 1);
    assert(run_case(&count) == -1 && count == SIZE_MAX);

    reset_fixture();
    set_page(0, FIXTURE_TYPE, 3, 1);
    set_page(1, FIXTURE_TYPE + 1, 4, 1);
    assert(run_case(&count) == -1 && count == SIZE_MAX);

    reset_fixture();
    shortread_page = 7;
    assert(run_case(&count) == -1 && count == SIZE_MAX);

    puts("PASS test-ublk-pageout-attribution");
    return 0;
}
