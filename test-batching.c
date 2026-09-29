/* Real codecs and NBD sockets, with device copies deferred until synchronize. */
#include "test-metadata.h"

struct CUstream_st {
    struct { void *dst; const void *src; size_t bytes; } copies[BATCH_DEPTH_MAX];
    unsigned pending, syncs, uploads, downloads;
    int fail_enqueue, fail_sync;
    pthread_barrier_t *read_gate;
};

static CUresult queue_copy(CUstream stream, void *dst, const void *src, size_t bytes)
{
    assert(stream && stream->pending < BATCH_DEPTH_MAX);
    if (stream->fail_enqueue && --stream->fail_enqueue == 0) return 1;
    unsigned i = stream->pending++;
    stream->copies[i].dst = dst;
    stream->copies[i].src = src;
    stream->copies[i].bytes = bytes;
    return CUDA_SUCCESS;
}

static CUresult upload(CUdeviceptr dst, const void *src, size_t bytes, CUstream stream)
{
    assert(dst >= g_vram_ptr && dst - g_vram_ptr + bytes <= g_vram_size);
    stream->uploads++;
    return queue_copy(stream, (void *)(uintptr_t)dst, src, bytes);
}

static CUresult download(void *dst, CUdeviceptr src, size_t bytes, CUstream stream)
{
    assert(src >= g_vram_ptr && src - g_vram_ptr + bytes <= g_vram_size);
    stream->downloads++;
    return queue_copy(stream, dst, (void *)(uintptr_t)src, bytes);
}

static CUresult synchronize(CUstream stream)
{
    assert(stream);
    stream->syncs++;
    if (stream->read_gate) pthread_barrier_wait(stream->read_gate);
    for (unsigned i = 0; i < stream->pending; i++)
        memcpy(stream->copies[i].dst, stream->copies[i].src, stream->copies[i].bytes);
    stream->pending = 0;
    return stream->fail_sync ? 1 : CUDA_SUCCESS;
}

static void init_worker(int idx)
{
    t_cstage = calloc(1, compression_stage_size());
    assert(t_cstage);
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
    metadata_heap_forbidden = 1;
}

static void pattern(char *page, unsigned seed)
{
    for (unsigned i = 0; i < COMP_PAGE; i++) page[i] = (char)(i % 67 + seed);
}

static void clear_store(CUstream stream)
{
    assert(!stream->pending);
    assert(comp_trim(0, (uint32_t)g_export_size, stream) == 0);
    assert(g_nfree_chunks == g_nchunks && !g_vram_obj_bytes);
    assert(!g_dedup_refs && !g_dedup_unique);
    *stream = (struct CUstream_st){0};
}

static void check_batching(CUstream stream)
{
    enum { REQUESTS = 40 };
    char input[REQUESTS * COMP_PAGE], output[REQUESTS * COMP_PAGE];
    struct bop ops[REQUESTS];
    for (int i = 0; i < REQUESTS; i++) {
        pattern(input + i * COMP_PAGE, (unsigned)i);
        ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * COMP_PAGE,
                              .len = COMP_PAGE, .slot = input + i * COMP_PAGE };
    }
    flush_packed_ops(ops, REQUESTS, stream);
    assert(stream->syncs == 2 && stream->uploads == REQUESTS && !stream->pending);
    for (int i = 0; i < REQUESTS; i++) {
        assert(!ops[i].error);
        ops[i].cmd = NBD_CMD_READ;
        ops[i].slot = output + i * COMP_PAGE;
    }
    flush_packed_ops(ops, REQUESTS, stream);
    assert(stream->syncs == 4 && !memcmp(input, output, sizeof(input)));
    clear_store(stream);

    /* Different pages can alias one lock stripe. They must form separate
     * groups, rather than recursively acquiring an exclusive rwlock. */
    ops[0] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = 0,
                           .len = COMP_PAGE, .slot = input };
    ops[1] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)NPLOCK * COMP_PAGE,
                           .len = COMP_PAGE, .slot = input + COMP_PAGE };
    flush_packed_ops(ops, 2, stream);
    assert(!ops[0].error && !ops[1].error && stream->syncs == 2);
    clear_store(stream);

    /* Request count alone does not bound scatter copies: each 64 KiB request
     * needs 16 pages. Five requests must run as 32 + 32 + 16 page groups. */
    char large[5 * BATCH_SLOT];
    for (int i = 0; i < 5 * BATCH_SLOT / COMP_PAGE; i++)
        pattern(large + i * COMP_PAGE, (unsigned)i);
    for (int i = 0; i < 5; i++)
        ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * BATCH_SLOT,
                              .len = BATCH_SLOT, .slot = large + i * BATCH_SLOT };
    flush_packed_ops(ops, 5, stream);
    assert(stream->syncs == 3 && stream->uploads == 80);
    for (int i = 0; i < 5; i++) assert(!ops[i].error);
    clear_store(stream);

    if (g_dedup) {
        for (int i = 0; i < COMP_BATCH; i++)
            ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * COMP_PAGE,
                                  .len = COMP_PAGE, .slot = input };
        flush_packed_ops(ops, COMP_BATCH, stream);
        assert(stream->syncs == 1 && stream->uploads == 1 && !stream->downloads);
        assert(g_dedup_unique == 1 && g_dedup_refs == COMP_BATCH);
        clear_store(stream);
    }
}

static void check_failures(CUstream stream)
{
    char a[COMP_PAGE], b[COMP_PAGE], out[COMP_PAGE];
    pattern(a, 1); pattern(b, 2);
    struct bop ops[2] = {
        { .cmd = NBD_CMD_WRITE, .offset = 0, .len = COMP_PAGE, .slot = a },
        { .cmd = NBD_CMD_WRITE, .offset = COMP_PAGE, .len = COMP_PAGE, .slot = b }
    };
    stream->fail_enqueue = 2;
    flush_packed_ops(ops, 2, stream);
    assert(ops[0].error == EIO && ops[1].error == EIO);
    assert(!stream->pending && stream->syncs == 1); /* first upload was drained */
    assert(g_ptes[0].kind == PTE_NONE && g_ptes[1].kind == PTE_NONE);
    assert(!g_vram_obj_bytes && !g_dedup_refs && !g_dedup_unique);
    clear_store(stream);

    assert(comp_write(0, a, COMP_PAGE, stream) == 0);
    uint64_t old = g_ptes[0].vram_off;
    ops[0].error = 0; ops[0].slot = b;
    stream->fail_sync = 1;
    flush_packed_ops(ops, 1, stream);
    assert(ops[0].error == EIO && g_ptes[0].vram_off == old && !stream->pending);
    stream->fail_sync = 0;
    assert(comp_read(0, out, COMP_PAGE, stream) == 0 && !memcmp(a, out, COMP_PAGE));
    stream->fail_sync = 1;
    ops[0].cmd = NBD_CMD_READ; ops[0].slot = out; ops[0].error = 0;
    flush_packed_ops(ops, 1, stream);
    assert(ops[0].error == EIO && !stream->pending);
    stream->fail_sync = 0;
    clear_store(stream);
}

static pthread_barrier_t readers_gate;

static void *shared_reader(void *arg)
{
    init_worker((int)(intptr_t)arg);
    struct CUstream_st stream = { .read_gate = &readers_gate };
    char page[COMP_PAGE], expected[COMP_PAGE];
    pattern(expected, 7);
    /* Both readers must reach DMA while holding the same page's read lock. */
    assert(comp_read(0, page, COMP_PAGE, &stream) == 0);
    assert(!memcmp(page, expected, COMP_PAGE));
    free(t_cstage);
    return NULL;
}

static pthread_barrier_t start_gate;

static void *contended_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    init_worker(idx);
    struct CUstream_st stream = {0};
    char page[COMP_PAGE], out[2 * COMP_PAGE], expected[COMP_PAGE];
    pthread_barrier_wait(&start_gate);
    for (int pass = 0; pass < 200; pass++) {
        if (idx < 2) {
            pattern(page, (unsigned)(1 + idx));
            /* Reverse request order on one connection, including lock stripes
             * 1023 and 0. Sorted acquisition must prevent a lock cycle. */
            uint64_t first = idx ? NPLOCK : NPLOCK - 1;
            uint64_t second = idx ? NPLOCK - 1 : NPLOCK;
            struct bop ops[2] = {
                { .cmd = NBD_CMD_WRITE, .offset = first * COMP_PAGE, .len = COMP_PAGE, .slot = page },
                { .cmd = NBD_CMD_WRITE, .offset = second * COMP_PAGE, .len = COMP_PAGE, .slot = page }
            };
            flush_packed_ops(ops, 2, &stream);
            assert(!ops[0].error && !ops[1].error);
        } else if (idx == 2) {
            assert(comp_read((NPLOCK - 1) * COMP_PAGE, out, sizeof(out), &stream) == 0);
            for (int i = 0; i < 2; i++) {
                unsigned seed = (unsigned char)out[i * COMP_PAGE];
                assert(seed <= 2);
                if (seed) pattern(expected, seed);
                else memset(expected, 0, sizeof(expected));
                assert(!memcmp(out + i * COMP_PAGE, expected, COMP_PAGE));
            }
        } else {
            assert(comp_trim((NPLOCK - 1) * COMP_PAGE, 2 * COMP_PAGE, &stream) == 0);
        }
        assert(!stream.pending);
    }
    free(t_cstage);
    return NULL;
}

static void check_concurrency(CUstream stream)
{
    char page[COMP_PAGE];
    pattern(page, 7);
    assert(comp_write(0, page, sizeof(page), stream) == 0);
    pthread_t threads[4];
    assert(pthread_barrier_init(&readers_gate, NULL, 2) == 0);
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, shared_reader, (void *)(intptr_t)i));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&readers_gate);
    clear_store(stream);
    assert(pthread_barrier_init(&start_gate, NULL, 4) == 0);
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, contended_worker, (void *)(intptr_t)i));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&start_gate);
    clear_store(stream);
}

struct connection { int fd[2], idx, result; pthread_t thread; struct CUstream_st stream; };

static void *serve_connection(void *arg)
{
    struct connection *c = arg;
    init_worker(c->idx);
    char *batch = calloc((size_t)g_batch_depth, BATCH_SLOT), *io = malloc(IO_BUF_SIZE);
    assert(batch && io);
    c->result = handle_client(c->fd[1], &c->stream, batch, io);
    assert(!c->stream.pending);
    close(c->fd[1]);
    free(batch); free(io); free(t_cstage);
    return NULL;
}

static void connect_client(struct connection *c, int idx)
{
    c->idx = idx;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, c->fd) == 0);
    assert(!pthread_create(&c->thread, NULL, serve_connection, c));
    unsigned char greeting[18];
    assert(recv_all(c->fd[0], greeting, sizeof(greeting)) == 0);
    uint32_t flags = htonl(NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);
    struct { uint64_t magic; uint32_t opt, len; } __attribute__((packed)) request = {
        htobe64(NBD_IHAVEOPT), htonl(NBD_OPT_EXPORT_NAME), 0
    };
    assert(send_all(c->fd[0], &flags, sizeof(flags)) == 0);
    assert(send_all(c->fd[0], &request, sizeof(request)) == 0);
    unsigned char info[10];
    assert(recv_all(c->fd[0], info, sizeof(info)) == 0);
}

static size_t append_request(char *packet, size_t n, uint16_t cmd, uint64_t offset,
                             uint32_t len, uint64_t handle, const char *payload)
{
    struct nbd_req_hdr h = { .magic = htonl(NBD_REQUEST_MAGIC), .type = htons(cmd),
                            .handle = handle, .from = htobe64(offset), .len = htonl(len) };
    memcpy(packet + n, &h, sizeof(h)); n += sizeof(h);
    if (cmd == NBD_CMD_WRITE) { memcpy(packet + n, payload, len); n += len; }
    return n;
}

static void response(int fd, uint64_t handle, uint32_t error, const char *expected, uint32_t len)
{
    struct nbd_resp_hdr r;
    assert(recv_all(fd, &r, sizeof(r)) == 0);
    assert(ntohl(r.magic) == NBD_RESPONSE_MAGIC && r.handle == handle && ntohl(r.error) == error);
    if (expected && !error) {
        char data[2 * BATCH_SLOT];
        assert(len <= sizeof(data) && recv_all(fd, data, len) == 0);
        assert(!memcmp(data, expected, len));
    }
}

static void disconnect_client(struct connection *c)
{
    char packet[sizeof(struct nbd_req_hdr)];
    size_t n = append_request(packet, 0, NBD_CMD_DISC, 0, 0, 0, NULL);
    assert(send_all(c->fd[0], packet, n) == 0);
    assert(!pthread_join(c->thread, NULL) && c->result == 0);
    close(c->fd[0]);
}

static void check_protocol(CUstream stream)
{
    char a[COMP_PAGE], b[COMP_PAGE], zero[COMP_PAGE] = {0}, packet[8 * COMP_PAGE];
    char patched[COMP_PAGE], large[BATCH_SLOT + COMP_PAGE] = {0};
    pattern(a, 11); pattern(b, 12);
    for (int mode = 0; mode < 3; mode++) {
        g_batch_enabled = mode != 0;
        g_batch_depth = mode == 1 ? 1 : BATCH_DEPTH_MAX;
        struct connection c = {0}, other = {0};
        connect_client(&c, 0); connect_client(&other, 1);
        unsigned long flushes = __atomic_load_n(&g_flush_count, __ATOMIC_RELAXED);
        size_t n = 0;
        n = append_request(packet, n, NBD_CMD_WRITE, 0, COMP_PAGE, 1, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 2, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, 0, COMP_PAGE, 3, b);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 4, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, COMP_PAGE, COMP_PAGE, 5, a);
        n = append_request(packet, n, NBD_CMD_TRIM, COMP_PAGE, COMP_PAGE, 6, NULL);
        n = append_request(packet, n, NBD_CMD_READ, COMP_PAGE, COMP_PAGE, 7, NULL);
        n = append_request(packet, n, NBD_CMD_FLUSH, 0, 0, 8, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, g_export_size, 512, 9, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 10, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, 33, 77, 11, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 12, NULL);
        n = append_request(packet, n, NBD_CMD_READ, 13, 123, 13, NULL);
        n = append_request(packet, n, NBD_CMD_READ, 0, sizeof(large), 14, NULL);
        assert(send_all(c.fd[0], packet, n) == 0);
        response(c.fd[0], 1, 0, NULL, 0); response(c.fd[0], 2, 0, a, COMP_PAGE);
        response(c.fd[0], 3, 0, NULL, 0); response(c.fd[0], 4, 0, b, COMP_PAGE);
        response(c.fd[0], 5, 0, NULL, 0); response(c.fd[0], 6, 0, NULL, 0);
        response(c.fd[0], 7, 0, zero, COMP_PAGE); response(c.fd[0], 8, 0, NULL, 0);
        response(c.fd[0], 9, EINVAL, NULL, 0); response(c.fd[0], 10, 0, b, COMP_PAGE);
        memcpy(patched, b, sizeof(patched)); memcpy(patched + 33, a, 77);
        memcpy(large, patched, sizeof(patched));
        response(c.fd[0], 11, 0, NULL, 0); response(c.fd[0], 12, 0, patched, COMP_PAGE);
        response(c.fd[0], 13, 0, patched + 13, 123);
        response(c.fd[0], 14, 0, large, sizeof(large));
        /* An acknowledged write must be visible over a different connection. */
        n = append_request(packet, 0, NBD_CMD_READ, 0, COMP_PAGE, 15, NULL);
        assert(send_all(other.fd[0], packet, n) == 0);
        response(other.fd[0], 15, 0, patched, COMP_PAGE);
        disconnect_client(&c); disconnect_client(&other);
        assert((__atomic_load_n(&g_flush_count, __ATOMIC_RELAXED) > flushes) == (mode != 0));
        clear_store(stream);
    }
}

static void check_protocol_errors(CUstream stream)
{
    char raw[COMP_PAGE], changed[COMP_PAGE], packet[2 * COMP_PAGE];
    uint32_t rng = 973;
    for (int i = 0; i < COMP_PAGE; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        raw[i] = (char)rng;
    }
    memcpy(changed, raw, sizeof(raw)); changed[0] ^= 1;
    assert(comp_write(0, raw, COMP_PAGE, stream) == 0 && g_ptes[0].kind == PTE_RAW);
    uint64_t reserved[1024];
    int count = 0;
    for (;;) {
        uint8_t klass = (uint8_t)class_for(COMP_PAGE);
        uint64_t off = pool_alloc(&klass);
        if (off == UINT64_MAX) break;
        assert(count < 1024);
        reserved[count++] = off;
    }
    assert(g_vram_obj_bytes == g_vram_size);
    struct connection c = {0};
    connect_client(&c, 0);
    size_t n = append_request(packet, 0, NBD_CMD_WRITE, 0, COMP_PAGE, 1, changed);
    n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 2, NULL);
    assert(send_all(c.fd[0], packet, n) == 0);
    response(c.fd[0], 1, ENOSPC, NULL, 0);
    response(c.fd[0], 2, 0, raw, COMP_PAGE);
    disconnect_client(&c);
    for (int i = 0; i < count; i++) pool_free(reserved[i], (uint8_t)class_for(COMP_PAGE));
    clear_store(stream);

    c = (struct connection){ .stream = { .fail_sync = 1 } };
    connect_client(&c, 0);
    n = append_request(packet, 0, NBD_CMD_WRITE, 0, COMP_PAGE, 3, raw);
    assert(send_all(c.fd[0], packet, n) == 0);
    response(c.fd[0], 3, EIO, NULL, 0);
    assert(!pthread_join(c.thread, NULL) && c.result == -1);
    char byte;
    assert(recv(c.fd[0], &byte, 1, 0) == 0);
    close(c.fd[0]);
    assert(g_ptes[0].kind == PTE_NONE);
    clear_store(stream);
}

int main(int argc, char **argv)
{
    assert(argc == 3 && !parse_compression(argv[1], &g_compress, &g_compress_level));
    g_dedup = atoi(argv[2]);
    g_batch_debug = 1;
    assert(packed_store_enabled());
    alarm(45); /* lock regressions must fail instead of hanging the suite */
    char tmp[] = "/tmp/nbd-vram-batching-test-XXXXXX";
    assert(mkdtemp(tmp) && chdir(tmp) == 0);
    g_nbd_threads = 4;
    g_vram_size = 4 * 1024 * 1024;
    g_export_size = 2 * g_vram_size;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    assert(g_vram_ptr && !compress_init());
    _cuMemcpyHtoDAsync = upload; _cuMemcpyDtoHAsync = download; _cuStreamSynchronize = synchronize;
    init_worker(0);
    struct CUstream_st stream = {0};
    check_batching(&stream);
    check_failures(&stream);
    check_concurrency(&stream);
    check_protocol(&stream);
    check_protocol_errors(&stream);
    free(t_cstage);
    metadata_heap_forbidden = 0;
    compress_shutdown();
    assert(!metadata_live_allocations);
    free((void *)(uintptr_t)g_vram_ptr);
    assert(!chdir("/") && !rmdir(tmp));
    printf("PASS batching %s dedup=%d: deferred DMA, sync counts, rollback, shared readers, contention, NBD ordering\n",
           argv[1], g_dedup);
}
