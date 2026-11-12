/* CPU-only target_io validation; no ublk library, device, worker or GPU. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int, char **) __asm__("unused_ublk_target_main");
#include "ublk-lzo-target.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static unsigned char dummy_buffer[COMP_PAGE];
static int completion_calls, completed_tag, completed_result, buffer_calls;

void *ublksrv_queue_get_io_buf(const struct ublksrv_queue *q, int tag)
{
    CHECK(q != NULL && tag >= 0);
    ++buffer_calls;
    return dummy_buffer;
}

int ublksrv_complete_io(const struct ublksrv_queue *q, unsigned tag, int result)
{
    CHECK(q != NULL);
    ++completion_calls;
    completed_tag = tag;
    completed_result = result;
    return 0;
}

static void check_request(unsigned op, unsigned flags, int expected, int tag)
{
    struct ublksrv_io_desc desc = {
        .op_flags = op | flags,
        .nr_sectors = 0,
        .start_sector = 0,
    };
    struct ublk_io_data io = { .tag = tag, .iod = &desc };
    struct ublksrv_queue queue = {0};
    int before_completions = completion_calls;
    int before_buffers = buffer_calls;

    CHECK(target_io(&queue, &io) == 0);
    CHECK(completion_calls == before_completions + 1);
    CHECK(completed_tag == tag && completed_result == expected);
    CHECK(buffer_calls == before_buffers + 1);
}

int request_flags_test_main(void) __asm__("main");
int request_flags_test_main(void)
{
    const unsigned accepted[] = {
        UBLK_IO_F_SWAP,
        UBLK_IO_F_SWAP | UBLK_IO_F_FAILFAST_DEV,
        UBLK_IO_F_SWAP | UBLK_IO_F_FAILFAST_TRANSPORT,
        UBLK_IO_F_SWAP | UBLK_IO_F_FAILFAST_DRIVER,
        UBLK_IO_F_FAILFAST_DEV | UBLK_IO_F_FAILFAST_TRANSPORT |
            UBLK_IO_F_FAILFAST_DRIVER,
    };
    const unsigned rejected[] = {
        UBLK_IO_F_FUA,
        UBLK_IO_F_META,
        UBLK_IO_F_SHMEM_ZC,
        1U << 12,
        UBLK_IO_F_SWAP | UBLK_IO_F_FAILFAST_DEV | UBLK_IO_F_FAILFAST_TRANSPORT |
            UBLK_IO_F_FAILFAST_DRIVER | (1U << 20),
    };
    int tag = 0;

    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        check_request(UBLK_IO_OP_READ, accepted[i], -EINVAL, tag++);
        check_request(UBLK_IO_OP_WRITE, accepted[i], -EINVAL, tag++);
    }
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        check_request(UBLK_IO_OP_READ, rejected[i], -EOPNOTSUPP, tag++);
        check_request(UBLK_IO_OP_WRITE, rejected[i], -EOPNOTSUPP, tag++);
    }
    check_request(UBLK_IO_OP_FLUSH, 0, -EOPNOTSUPP, tag++);
    CHECK(completion_calls == tag);
    puts("UBLK_REQUEST_FLAGS_VALIDATION_OK");
    return 0;
}
