/* nbd-vram.c - NBD server backed by GPU VRAM via CUDA
 *
 * Implements NBD fixed-newstyle protocol over a Unix socket.
 * No NVIDIA P2P or kernel symbols needed - uses cuMemcpyHtoDAsync/DtoHAsync.
 *
 * Optional lz4/lzo/zstd compression and exact page deduplication pack swap pages
 * in VRAM so the NBD export can be larger than the CUDA allocation.
 * Loads the selected codec at runtime; deduplication also works without a codec.
 *
 * Compile: gcc -O2 -o nbd-vram nbd-vram.c -ldl -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <linux/fs.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <limits.h>
#include <arpa/inet.h>
#include <endian.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <time.h>
#include "backing-topology.h"

/* PR_SET_IO_FLUSHER (Linux 5.6+) may be absent from older headers */
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

/* -------------------------------------------------------------------------
 * CUDA driver API (dynamic load)
 * ---------------------------------------------------------------------- */

typedef int                CUresult;
typedef int                CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st    *CUcontext;
typedef struct CUstream_st *CUstream;

#define CUDA_SUCCESS           0
#define CU_CTX_SCHED_AUTO      0
#define CU_CTX_SCHED_BLOCKING_SYNC 4
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuCtxSetCurrent)(CUcontext);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoDAsync)(CUdeviceptr, const void *, size_t, CUstream);
typedef CUresult (*pfn_cuMemcpyDtoHAsync)(void *, CUdeviceptr, size_t, CUstream);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned int);
typedef CUresult (*pfn_cuStreamDestroy)(CUstream);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuMemAllocHost)(void **, size_t);
typedef CUresult (*pfn_cuMemFreeHost)(void *);
typedef CUresult (*pfn_cuGetErrorString)(CUresult, const char **);

static void                  *g_libcuda;
static pfn_cuInit              _cuInit;
static pfn_cuDeviceGet         _cuDeviceGet;
static pfn_cuCtxCreate         _cuCtxCreate;
static pfn_cuCtxDestroy        _cuCtxDestroy;
static pfn_cuCtxSetCurrent     _cuCtxSetCurrent;
static pfn_cuMemAlloc          _cuMemAlloc;
static pfn_cuMemFree           _cuMemFree;
static pfn_cuMemcpyHtoDAsync   _cuMemcpyHtoDAsync;
static pfn_cuMemcpyDtoHAsync   _cuMemcpyDtoHAsync;
static pfn_cuStreamCreate      _cuStreamCreate;
static pfn_cuStreamDestroy     _cuStreamDestroy;
static pfn_cuStreamSynchronize _cuStreamSynchronize;
static pfn_cuMemAllocHost      _cuMemAllocHost;
static pfn_cuMemFreeHost       _cuMemFreeHost;
static pfn_cuGetErrorString    _cuGetErrorString;

#define LOAD_SYM(h, name, pfn) do { \
    pfn = dlsym(h, name); \
    if (!pfn) { fprintf(stderr, "dlsym(%s) failed\n", name); return -1; } \
} while (0)

static int load_libcuda_from(const char *library) {
    const char *defaults[] = { "libcuda.so.1",
                             "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
                             "/usr/lib64/libcuda.so.1", NULL };
    const char *explicit_path[] = { library, NULL };
    const char **paths = library ? explicit_path : defaults;
    for (int i = 0; paths[i]; i++) {
        g_libcuda = dlopen(paths[i], RTLD_NOW);
        if (g_libcuda) { printf("[nbd-vram] loaded %s\n", paths[i]); break; }
    }
    if (!g_libcuda) { fprintf(stderr, "[nbd-vram] cannot load libcuda.so.1\n"); return -1; }
    LOAD_SYM(g_libcuda, "cuInit",                _cuInit);
    LOAD_SYM(g_libcuda, "cuDeviceGet",           _cuDeviceGet);
    LOAD_SYM(g_libcuda, "cuCtxCreate_v2",        _cuCtxCreate);
    LOAD_SYM(g_libcuda, "cuCtxDestroy_v2",       _cuCtxDestroy);
    LOAD_SYM(g_libcuda, "cuCtxSetCurrent",       _cuCtxSetCurrent);
    LOAD_SYM(g_libcuda, "cuMemAlloc_v2",         _cuMemAlloc);
    LOAD_SYM(g_libcuda, "cuMemFree_v2",          _cuMemFree);
    LOAD_SYM(g_libcuda, "cuMemcpyHtoDAsync_v2",  _cuMemcpyHtoDAsync);
    LOAD_SYM(g_libcuda, "cuMemcpyDtoHAsync_v2",  _cuMemcpyDtoHAsync);
    LOAD_SYM(g_libcuda, "cuStreamCreate",         _cuStreamCreate);
    LOAD_SYM(g_libcuda, "cuStreamDestroy_v2",     _cuStreamDestroy);
    LOAD_SYM(g_libcuda, "cuStreamSynchronize",    _cuStreamSynchronize);
    LOAD_SYM(g_libcuda, "cuMemAllocHost_v2",      _cuMemAllocHost);
    LOAD_SYM(g_libcuda, "cuMemFreeHost",          _cuMemFreeHost);
    LOAD_SYM(g_libcuda, "cuGetErrorString",       _cuGetErrorString);
    return 0;
}

static int load_libcuda(void) { return load_libcuda_from(NULL); }

static const char *cuda_err(CUresult r) {
    const char *s = NULL;
    if (_cuGetErrorString) _cuGetErrorString(r, &s);
    return s ? s : "unknown";
}

#define CUDA_CHECK(call) do { \
    CUresult _r = (call); \
    if (_r != CUDA_SUCCESS) { \
        fprintf(stderr, "[nbd-vram] " #call " failed: %s (%d)\n", cuda_err(_r), _r); \
        return -1; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic */
#define NBD_MAGIC_INIT     UINT64_C(0x4e42444d41474943)  /* "NBDMAGIC" */
#define NBD_IHAVEOPT       UINT64_C(0x49484156454f5054)  /* "IHAVEOPT" */
#define NBD_OPT_REP_MAGIC  UINT64_C(0x3e889045565a9)

/* Server handshake flags */
#define NBD_FLAG_FIXED_NEWSTYLE  0x0001
#define NBD_FLAG_NO_ZEROES       0x0002

/* Client handshake flags */
#define NBD_FLAG_C_FIXED_NEWSTYLE 0x00000001
#define NBD_FLAG_C_NO_ZEROES      0x00000002

/* Options (client→server) */
#define NBD_OPT_EXPORT_NAME  1
#define NBD_OPT_ABORT        2
#define NBD_OPT_LIST         3
#define NBD_OPT_INFO         6
#define NBD_OPT_GO           7

/* Option replies (server→client) */
#define NBD_REP_ACK          1
#define NBD_REP_SERVER       2
#define NBD_REP_INFO         3
#define NBD_REP_FLAG_ERROR   UINT32_C(0x80000000)
#define NBD_REP_ERR_UNSUP    (NBD_REP_FLAG_ERROR | 1)
#define NBD_REP_ERR_INVALID  (NBD_REP_FLAG_ERROR | 3)

/* Info types */
#define NBD_INFO_EXPORT      0

/* Transmission flags (per-export) */
#define NBD_FLAG_HAS_FLAGS      0x0001
#define NBD_FLAG_SEND_FLUSH     0x0004
#define NBD_FLAG_SEND_TRIM      0x0020
#define NBD_FLAG_CAN_MULTI_CONN 0x0100

/* Transmission request magic */
#define NBD_REQUEST_MAGIC    0x25609513
#define NBD_RESPONSE_MAGIC   0x67446698

/* Commands */
#define NBD_CMD_READ         0
#define NBD_CMD_WRITE        1
#define NBD_CMD_DISC         2
#define NBD_CMD_FLUSH        3
#define NBD_CMD_TRIM         4

/* 28-byte NBD request header, shared by the per-op and batched paths */
struct nbd_req_hdr {
    uint32_t magic;
    uint16_t flags;
    uint16_t type;
    uint64_t handle;   /* opaque, echoed back verbatim (stays network order) */
    uint64_t from;
    uint32_t len;
} __attribute__((packed));

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

static int recv_all(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int drain(int fd, uint32_t len) {
    char buf[4096];
    while (len > 0) {
        uint32_t chunk = (len > sizeof(buf)) ? sizeof(buf) : len;
        if (recv_all(fd, buf, chunk) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * NBD option reply helpers
 * ---------------------------------------------------------------------- */

static int send_opt_reply(int fd, uint32_t opt, uint32_t reply_type,
                           const void *data, uint32_t data_len)
{
    struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply_type;
        uint32_t len;
    } __attribute__((packed)) hdr;

    hdr.magic      = htobe64(NBD_OPT_REP_MAGIC);
    hdr.opt        = htonl(opt);
    hdr.reply_type = htonl(reply_type);
    hdr.len        = htonl(data_len);

    if (send_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (data_len > 0 && send_all(fd, data, data_len) != 0) return -1;
    return 0;
}

static int send_export_info(int fd, uint32_t opt, uint64_t size, uint16_t tx_flags)
{
    struct {
        uint16_t info_type;   /* NBD_INFO_EXPORT = 0 */
        uint64_t export_size;
        uint16_t tx_flags;
    } __attribute__((packed)) info;

    info.info_type   = htons(NBD_INFO_EXPORT);
    info.export_size = htobe64(size);
    info.tx_flags    = htons(tx_flags);

    return send_opt_reply(fd, opt, NBD_REP_INFO, &info, sizeof(info));
}

/* Parse INFO/GO without allocating memory during swap recovery. */
static int read_info_option(int fd, uint32_t len)
{
    uint32_t name_net;
    uint16_t count_net;
    if (len < 6) return drain(fd, len) == 0 ? 0 : -1;
    if (recv_all(fd, &name_net, sizeof(name_net)) != 0) return -1;
    len -= sizeof(name_net);
    uint32_t name_len = ntohl(name_net);
    if (name_len > len - sizeof(count_net)) return drain(fd, len) == 0 ? 0 : -1;
    if (drain(fd, name_len) != 0) return -1;
    len -= name_len;
    if (recv_all(fd, &count_net, sizeof(count_net)) != 0) return -1;
    len -= sizeof(count_net);
    if (len != (uint32_t)ntohs(count_net) * 2) return drain(fd, len) == 0 ? 0 : -1;
    return drain(fd, len) == 0 ? 1 : -1;
}

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle handshake
 * ---------------------------------------------------------------------- */

static int nbd_handshake(int fd, uint64_t export_size, int send_trim)
{
    /* Phase 1: server greeting */
    struct {
        uint64_t magic1;
        uint64_t magic2;
        uint16_t srv_flags;
    } __attribute__((packed)) greeting;

    greeting.magic1    = htobe64(NBD_MAGIC_INIT);
    greeting.magic2    = htobe64(NBD_IHAVEOPT);
    greeting.srv_flags = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (send_all(fd, &greeting, sizeof(greeting)) != 0) return -1;

    /* Phase 2: client flags */
    uint32_t client_flags_net;
    if (recv_all(fd, &client_flags_net, 4) != 0) return -1;
    uint32_t client_flags = ntohl(client_flags_net);
    if (!(client_flags & NBD_FLAG_C_FIXED_NEWSTYLE) ||
        (client_flags & ~(NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES))) return -1;
    int no_zeroes = !!(client_flags & NBD_FLAG_C_NO_ZEROES);

    /* Phase 3: option haggling */
    for (;;) {
        struct {
            uint64_t ihaveopt;
            uint32_t opt;
            uint32_t opt_len;
        } __attribute__((packed)) opt_hdr;

        if (recv_all(fd, &opt_hdr, sizeof(opt_hdr)) != 0) return -1;
        if (be64toh(opt_hdr.ihaveopt) != NBD_IHAVEOPT) return -1;

        uint32_t opt     = ntohl(opt_hdr.opt);
        uint32_t opt_len = ntohl(opt_hdr.opt_len);

        /* Limit option payload to something sane */
        if (opt_len > 65536) return -1;

        /* VRAM and its location map are volatile; do not advertise durable FLUSH. */
        uint16_t tx_flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_CAN_MULTI_CONN;
        if (send_trim)
            tx_flags |= NBD_FLAG_SEND_TRIM;

        switch (opt) {
        case NBD_OPT_EXPORT_NAME:
            /* Drain the export name (we only have one export) */
            if (drain(fd, opt_len) != 0) return -1;
            /* Reply: export size + tx_flags [+ 124 zeros if needed] */
            {
                struct {
                    uint64_t size;
                    uint16_t tx_flags;
                } __attribute__((packed)) info;
                info.size     = htobe64(export_size);
                info.tx_flags = htons(tx_flags);
                if (send_all(fd, &info, sizeof(info)) != 0) return -1;
                if (!no_zeroes) {
                    char zeros[124] = {0};
                    if (send_all(fd, zeros, sizeof(zeros)) != 0) return -1;
                }
            }
            return 0;  /* transmission begins */

        case NBD_OPT_GO:
        case NBD_OPT_INFO:
            {
            int valid = read_info_option(fd, opt_len);
            if (valid < 0) return -1;
            if (!valid) {
                if (send_opt_reply(fd, opt, NBD_REP_ERR_INVALID, NULL, 0) != 0) return -1;
                break;
            }
            if (send_export_info(fd, opt, export_size, tx_flags) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            if (opt == NBD_OPT_GO)
                return 0;  /* transmission begins */
            break;
            }

        case NBD_OPT_LIST:
            /* One anonymous export */
            if (drain(fd, opt_len) != 0) return -1;
            {
                uint32_t name_len = htonl(0);
                if (send_opt_reply(fd, opt, NBD_REP_SERVER, &name_len, 4) != 0)
                    return -1;
            }
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            break;

        case NBD_OPT_ABORT:
            drain(fd, opt_len);
            send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0);
            return -1;

        default:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ERR_UNSUP, NULL, 0) != 0)
                return -1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Transmission loop
 * ---------------------------------------------------------------------- */

#define DEFAULT_SIZE_MB 7168
#define SIZE_ALIGN      (64 * 1024)
#define SOCK_PATH       "/run/nbd-vram.sock"
#define IO_BUF_SIZE     (64 * 1024)
#define NBD_THREADS_MAX     64
#define NBD_THREADS_DEFAULT  2

/* Bound both request count and bytes queued before replying. Small bursts limit
 * copy-engine occupancy on GPUs also driving a display. Larger requests use
 * the per-op path, synchronizing each IO_BUF_SIZE chunk before submitting more. */
#define BATCH_SLOT          (64 * 1024)   /* max per-request size that batches */
#define BATCH_BYTES_MAX     (64 * 1024)   /* total payload per worker/batch */
#define BATCH_DEPTH_DEFAULT 8
#define BATCH_DEPTH_MAX     256

static CUdeviceptr  g_vram_ptr;
static uint64_t     g_vram_size;
static uint64_t     g_export_size;           /* logical NBD size, may exceed physical VRAM */
enum compression { COMP_OFF, COMP_LZ4, COMP_ZSTD, COMP_LZO };
static int          g_compress;              /* enum compression */
static int          g_dedup;                 /* VRAM_DEDUP=1 */
static int          g_compress_level = 3;     /* zstd level, 1..22 */
static int          g_compress_ratio_tenths = 20; /* VRAM_COMPRESS_RATIO in 0.1x units */
static CUcontext    g_cu_ctx;
static int          g_listen_fd  = -1;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal handler requires lock-free atomic int");
static atomic_int g_running = 1;
static atomic_int g_term_requested = 0;
static atomic_int g_worker_error = 0;
static int          g_nbd_threads = NBD_THREADS_DEFAULT;
static int g_client_fds[NBD_THREADS_MAX];
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_unexpected_client_loss;
static int          g_batch_enabled = 1;                  /* VRAM_BATCH=0 disables */
static int          g_batch_depth   = BATCH_DEPTH_DEFAULT; /* VRAM_BATCH_DEPTH */
static int          g_batch_debug   = 0;                  /* VRAM_BATCH_DEBUG=1 */
static unsigned long g_batch_count   = 0;                  /* flushes that coalesced (n>1) */
static unsigned long g_batch_ops     = 0;                  /* ops in those n>1 flushes */
static unsigned long g_flush_count   = 0;                  /* every batched-path flush, incl n==1 */
static unsigned long g_flush_ops     = 0;                  /* ops across all flushes (true depth) */
static unsigned long g_legacy_ops    = 0;                  /* READ/WRITE via the per-op path */

/* Keep the original 0/1 interface while allowing an explicit codec and level. */
static int parse_compression(const char *s, int *codec, int *level)
{
    int c = COMP_OFF, l = 3;
    if (!s || strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
        c = COMP_OFF;
    } else if (strcmp(s, "1") == 0 || strcmp(s, "lz4") == 0) {
        c = COMP_LZ4;
    } else if (strcmp(s, "lzo") == 0) {
        c = COMP_LZO;
    } else if (strcmp(s, "zstd") == 0) {
        c = COMP_ZSTD;
    } else if (strncmp(s, "zstd:", 5) == 0) {
        const char *p = s + 5;
        if (*p < '1' || *p > '9') return -1;
        l = 0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            l = l * 10 + (*p - '0');
            if (l > 22) return -1;
        }
        c = COMP_ZSTD;
    } else {
        return -1;
    }
    *codec = c;
    *level = l;
    return 0;
}

static const char *compression_name(void)
{
    return g_compress == COMP_ZSTD ? "zstd" : g_compress == COMP_LZ4 ? "lz4" :
           g_compress == COMP_LZO ? "lzo" : "off";
}

static int packed_store_enabled(void)
{
    return g_compress != COMP_OFF || g_dedup;
}

/* Parse X or X.Y where X is 1..8 and Y is one decimal digit. */
static int parse_ratio_tenths(const char *s, int *out)
{
    if (!s || !*s) return -1;
    const char *p = s;
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }
    int frac = 0;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        frac = *p - '0';
        p++;
    }
    if (*p != '\0') return -1;
    int tenths = whole * 10 + frac;
    if (tenths < 10 || tenths > 80) return -1;
    *out = tenths;
    return 0;
}

/* Caller holds g_client_lock, including while accepting a new client. */
static int clients_connected_locked(void) {
    for (int i = 0; i < g_nbd_threads; i++)
        if (g_client_fds[i] >= 0) return 1;
    return 0;
}

static void retire_client_locked(int idx, int result)
{
    if (result != 0) g_unexpected_client_loss = 1;
    g_client_fds[idx] = -1;
}

static void sig_handler(int sig) {
    (void)sig;
    atomic_store_explicit(&g_term_requested, 1, memory_order_relaxed);
}

/* shortcut: any active swap prevents drain; relax only with stable kernel identity proof. */
static int swaps_fd_empty(int fd)
{
    char buf[1024], header[128];
    size_t used = 0;
    int header_done = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 0;
        if (!n) return header_done;
        for (ssize_t i = 0; i < n; i++) {
            if (header_done || !buf[i] || used == sizeof(header) - 1) return 0;
            if (buf[i] != '\n') { header[used++] = buf[i]; continue; }
            header[used] = '\0';
            char *p = header;
            const char *names[] = { "Filename", "Type", "Size", "Used", "Priority" };
            for (int j = 0; j < 5; j++) {
                while (*p == ' ' || *p == '\t') p++;
                size_t len = strlen(names[j]);
                if (strncmp(p, names[j], len) || (p[len] && p[len] != ' ' && p[len] != '\t'))
                    return 0;
                p += len;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p) return 0;
            header_done = 1;
        }
    }
}

static int swaps_empty(void)
{
    struct statfs procfs;
    int fd = open("/proc/swaps", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int safe = fstatfs(fd, &procfs) == 0 && procfs.f_type == PROC_SUPER_MAGIC &&
               swaps_fd_empty(fd);
    close(fd);
    return safe;
}

#ifndef NBD_DRAIN_CHECK
#define NBD_DRAIN_CHECK swaps_empty
#endif

static int finish_drain_checked(int (*independent)(void))
{
    pthread_mutex_lock(&g_client_lock);
    /* shortcut: unexpected loss retains VRAM until an authoritative drain protocol exists. */
    int idle = !clients_connected_locked() && !g_unexpected_client_loss && independent() == 1;
    if (idle) atomic_store(&g_running, 0);
    pthread_mutex_unlock(&g_client_lock);
    return idle;
}

static int finish_drain_if_idle(void)
{
    return finish_drain_checked(NBD_DRAIN_CHECK);
}

static void worker_fatal_error(const char *operation, int error)
{
    fprintf(stderr, "[nbd-vram] %s failed (%s); draining active clients\n", operation, strerror(error));
    atomic_store(&g_worker_error, 1);
    atomic_store(&g_term_requested, 1);
}

/* One NBD response header */
struct nbd_resp_hdr {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
} __attribute__((packed));

/* One batched op: a READ or WRITE that fits in a slot of the pinned batch buffer */
struct bop {
    uint64_t handle;   /* network order, echoed verbatim */
    uint64_t offset;
    uint32_t len;
    uint16_t cmd;
    uint32_t error;
    char    *slot;     /* host staging memory for this op */
};

/* Overflow-safe bounds check against the NBD export. Written as
 * offset > size || length > size - offset so that a near-2^64 offset cannot wrap
 * the sum and slip past, which the naive offset + length > size would allow. */
static inline int oob(uint64_t offset, uint32_t length) {
    return offset > g_export_size || (uint64_t)length > g_export_size - offset;
}

/* -------------------------------------------------------------------------
 * Optional packed VRAM store (compression and/or deduplication)
 *
 * Logical 4K pages are packed into the CUDA allocation. The NBD export is
 * VRAM_COMPRESS_RATIO times the physical size. Same-filled pages (zeros) take
 * no VRAM. TRIM frees slots so the pool can refill; swapon --discard=pages.
 * ---------------------------------------------------------------------- */

#define COMP_PAGE    4096
#define COMP_BATCH   32
#define SLAB_SZ      (64 * 1024)
#define NPLOCK       1024
#ifndef STATUS_PATH
#define STATUS_PATH  "/run/nbd-vram.status"
#endif
#define STATUS_TMP   STATUS_PATH ".tmp"

#define PTE_NONE       0
#define PTE_COMPRESSED 1
#define PTE_RAW        2
#define PTE_SAME       3
#define PTE_DISK       4

static const uint16_t k_class_sz[] = {
    32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
    512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096
};
#define NCLASS ((int)(sizeof(k_class_sz) / sizeof(k_class_sz[0])))
#define SLAB_NONE UINT32_MAX
#define SLAB_BITMAP_BYTES ((SLAB_SZ / 32 + 7) / 8)

struct pte {
    uint64_t vram_off;
    uint64_t checksum;
    uint16_t clen;
    uint8_t  kind;
    uint8_t  fill;
    uint8_t  klass;
};

struct slab {
    uint32_t chunk;
    uint32_t next, prev; /* partial-class list; next also links unused slabs */
    uint16_t nobj;
    uint16_t nfree;
    uint16_t hint;
    uint8_t  klass;
    uint8_t bm[SLAB_BITMAP_BYTES]; /* sized for the smallest class; 1 = in use */
};

struct cpend {
    const char *plain; /* valid until commit, used for exact dedup comparison */
    uint64_t hash;
    uint64_t pg;
    uint64_t new_off;
    uint64_t old_off;
    uint16_t clen;
    uint8_t  kind, fill, klass, slot;
    uint8_t  old_kind, old_klass;
};

struct rpend {
    uint64_t pg;
    uint64_t vram_off;
    uint64_t checksum;
    char *dst;
    uint16_t clen;
    uint8_t  kind, slot;
};

typedef int (*pfn_LZ4_compress_default)(const char *, char *, int, int);
typedef int (*pfn_LZ4_decompress_safe)(const char *, char *, int, int);

static void                      *g_liblz4;
static pfn_LZ4_compress_default   _LZ4_compress_default;
static pfn_LZ4_decompress_safe    _LZ4_decompress_safe;

/* liblzo's lzo_uint is size_t; workspace size follows LZO1X_1_MEM_COMPRESS. */
#define LZO_WORK_SIZE (16384 * sizeof(void *))
#define LZO_BOUND (COMP_PAGE + COMP_PAGE / 16 + 64 + 3)
static void *g_liblzo;
static int (*_lzo_compress)(const unsigned char *, size_t, unsigned char *, size_t *, void *);
static int (*_lzo_decompress)(const unsigned char *, size_t, unsigned char *, size_t *, void *);
static void *g_lzo_work[NBD_THREADS_MAX];
static __thread void *t_lzo_work;
static __thread unsigned char t_lzo_out[LZO_BOUND];
static void *metadata_calloc(size_t count, size_t size);

/* Opaque stable-API types keep codec development headers optional. */
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
static void *g_libzstd;
static ZSTD_CCtx *(*_ZSTD_createCCtx)(void);
static ZSTD_DCtx *(*_ZSTD_createDCtx)(void);
static size_t (*_ZSTD_freeCCtx)(ZSTD_CCtx *);
static size_t (*_ZSTD_freeDCtx)(ZSTD_DCtx *);
static size_t (*_ZSTD_compressCCtx)(ZSTD_CCtx *, void *, size_t, const void *, size_t, int);
static size_t (*_ZSTD_decompressDCtx)(ZSTD_DCtx *, void *, size_t, const void *, size_t);
static unsigned (*_ZSTD_isError)(size_t);
static int (*_ZSTD_maxCLevel)(void);
static ZSTD_CCtx *g_zstd_cctx[NBD_THREADS_MAX];
static ZSTD_DCtx *g_zstd_dctx[NBD_THREADS_MAX];
static __thread ZSTD_CCtx *t_zstd_cctx;
static __thread ZSTD_DCtx *t_zstd_dctx;

static struct pte     *g_ptes;
static int             g_back_fd = -1;
static __thread char   t_disk_page[COMP_PAGE] __attribute__((aligned(4096)));
static uint64_t        g_npages;
static struct slab    *g_slabs;
static uint32_t        g_class_head[NCLASS];
static uint32_t        g_nchunks, g_nfree_chunks, g_free_chunk = SLAB_NONE;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
/* Changed only while all requests are quiesced, under g_alloc_lock. */
static int g_pool_retired;
static void (*g_retired_slot_freed)(uint64_t offset);
static int (*g_prepare_slot)(uint64_t offset);
static pthread_rwlock_t g_plock[NPLOCK];
static unsigned long   g_comp_enospc;
static unsigned long   g_pages_compressed, g_pages_raw, g_pages_same, g_pages_disk;
static uint64_t        g_vram_obj_bytes;   /* allocated object bytes, under g_alloc_lock */

/* Dedup objects are immutable VRAM payloads. The index and all reference
 * counts are protected by g_dedup_lock. Lock order is page -> dedup -> allocator;
 * never acquire a page lock while holding either global lock. Readers need only
 * their page lock: its reference keeps the immutable payload alive.
 * Keep index allocations optional so disabled dedup adds no per-page metadata. */
struct dedup_obj {
    uint64_t hash, vram_off, refs;
    struct dedup_obj *next, **prev;
    uint16_t clen;
    uint8_t kind, klass;
};
static struct dedup_obj **g_dedup_index, **g_dedup_ptes;
static struct dedup_obj *g_dedup_objects, *g_dedup_free;
static size_t g_dedup_capacity;
static size_t g_dedup_buckets;
static pthread_mutex_t g_dedup_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_dedup_unique, g_dedup_refs, g_dedup_saved_bytes, g_dedup_hits;
static __thread char t_dedup_plain[COMP_PAGE];

static uint64_t dedup_metadata_bytes(void)
{
    return g_dedup ? g_dedup_buckets * sizeof(*g_dedup_index) +
                     g_npages * sizeof(*g_dedup_ptes) +
                     g_dedup_capacity * sizeof(*g_dedup_objects) : 0;
}

static __thread char *t_cstage;
static __thread int   t_cstage_cuda;
static __thread char  t_page[COMP_PAGE] __attribute__((aligned(16)));

static size_t compression_stage_size(void)
{
    return (size_t)(COMP_BATCH + (g_dedup ? 1 : 0)) * COMP_PAGE;
}

/* FNV-1a is only a lookup accelerator, never proof of equality. */
static uint64_t page_hash(const char *plain)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < COMP_PAGE; i++) {
        hash ^= (unsigned char)plain[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int load_liblz4(void)
{
    const char *paths[] = {
        "liblz4.so.1",
        "/usr/lib/x86_64-linux-gnu/liblz4.so.1",
        "/usr/lib64/liblz4.so.1",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        g_liblz4 = dlopen(paths[i], RTLD_NOW);
        if (g_liblz4) {
            printf("[nbd-vram] loaded %s\n", paths[i]);
            break;
        }
    }
    if (!g_liblz4) {
        fprintf(stderr, "[nbd-vram] lz4 selected but cannot load liblz4.so.1 - install liblz4-1 (Debian) or lz4-libs (Fedora)\n");
        return -1;
    }
    _LZ4_compress_default = (pfn_LZ4_compress_default)dlsym(g_liblz4, "LZ4_compress_default");
    _LZ4_decompress_safe  = (pfn_LZ4_decompress_safe)dlsym(g_liblz4, "LZ4_decompress_safe");
    if (!_LZ4_compress_default || !_LZ4_decompress_safe) {
        fprintf(stderr, "[nbd-vram] liblz4 missing LZ4_compress_default/LZ4_decompress_safe\n");
        return -1;
    }
    return 0;
}

static int load_libzstd(void)
{
    g_libzstd = dlopen("libzstd.so.1", RTLD_NOW);
    if (!g_libzstd) {
        fprintf(stderr, "[nbd-vram] zstd selected but cannot load libzstd.so.1 - install libzstd1 (Debian) or libzstd (Fedora)\n");
        return -1;
    }
    LOAD_SYM(g_libzstd, "ZSTD_createCCtx", _ZSTD_createCCtx);
    LOAD_SYM(g_libzstd, "ZSTD_createDCtx", _ZSTD_createDCtx);
    LOAD_SYM(g_libzstd, "ZSTD_freeCCtx", _ZSTD_freeCCtx);
    LOAD_SYM(g_libzstd, "ZSTD_freeDCtx", _ZSTD_freeDCtx);
    LOAD_SYM(g_libzstd, "ZSTD_compressCCtx", _ZSTD_compressCCtx);
    LOAD_SYM(g_libzstd, "ZSTD_decompressDCtx", _ZSTD_decompressDCtx);
    LOAD_SYM(g_libzstd, "ZSTD_isError", _ZSTD_isError);
    LOAD_SYM(g_libzstd, "ZSTD_maxCLevel", _ZSTD_maxCLevel);
    if (g_compress_level > _ZSTD_maxCLevel()) {
        fprintf(stderr, "[nbd-vram] zstd level %d exceeds this library's maximum %d\n",
                g_compress_level, _ZSTD_maxCLevel());
        return -1;
    }
    /* Allocate and warm one context pair per worker before advertising readiness.
     * All requests use fixed 4K pages, so the workspace can be reused under swap
     * pressure without creating a new context for every page. */
    char plain[COMP_PAGE] = {0}, packed[2 * COMP_PAGE];
    for (int i = 0; i < g_nbd_threads; i++) {
        g_zstd_cctx[i] = _ZSTD_createCCtx();
        g_zstd_dctx[i] = _ZSTD_createDCtx();
        if (!g_zstd_cctx[i] || !g_zstd_dctx[i]) {
            fprintf(stderr, "[nbd-vram] zstd context allocation failed\n");
            return -1;
        }
        size_t n = _ZSTD_compressCCtx(g_zstd_cctx[i], packed, sizeof(packed),
                                    plain, sizeof(plain), g_compress_level);
        if (_ZSTD_isError(n) ||
            _ZSTD_decompressDCtx(g_zstd_dctx[i], plain, sizeof(plain), packed, n) != COMP_PAGE) {
            fprintf(stderr, "[nbd-vram] zstd context initialization failed\n");
            return -1;
        }
    }
    printf("[nbd-vram] loaded libzstd.so.1 (level %d)\n", g_compress_level);
    return 0;
}

static int load_liblzo(void)
{
    unsigned (*version)(void);
    int (*init)(unsigned, int, int, int, int, int, int, int, int, int);
    struct lzo_callback {
        void *(*alloc)(void *, size_t, size_t);
        void (*free_fn)(void *, void *);
        void (*progress)(void *, size_t, size_t, int);
        void *user1;
        size_t user2, user3;
    };
    g_liblzo = dlopen("liblzo2.so.2", RTLD_NOW);
    if (!g_liblzo) {
        fprintf(stderr, "[nbd-vram] lzo selected but cannot load liblzo2.so.2\n");
        return -1;
    }
    LOAD_SYM(g_liblzo, "lzo_version", version);
    LOAD_SYM(g_liblzo, "__lzo_init_v2", init);
    LOAD_SYM(g_liblzo, "lzo1x_1_compress", _lzo_compress);
    LOAD_SYM(g_liblzo, "lzo1x_decompress_safe", _lzo_decompress);
    if (init(version(), sizeof(short), sizeof(int), sizeof(long), sizeof(uint32_t),
             sizeof(size_t), sizeof(void *), sizeof(char *), sizeof(void *),
             sizeof(struct lzo_callback)) != 0) {
        fprintf(stderr, "[nbd-vram] liblzo ABI initialization failed\n");
        return -1;
    }
    unsigned char plain[COMP_PAGE], decoded[COMP_PAGE], packed[LZO_BOUND];
    for (size_t i = 0; i < sizeof(plain); i++) plain[i] = (unsigned char)(i % 71);
    for (int i = 0; i < g_nbd_threads; i++) {
        g_lzo_work[i] = metadata_calloc(1, LZO_WORK_SIZE);
        if (!g_lzo_work[i]) { perror("lzo workspace"); return -1; }
        size_t n = sizeof(packed), out = sizeof(decoded);
        if (_lzo_compress(plain, sizeof(plain), packed, &n, g_lzo_work[i]) != 0 ||
            n > sizeof(packed) || _lzo_decompress(packed, n, decoded, &out, NULL) != 0 ||
            out != sizeof(decoded) || memcmp(plain, decoded, sizeof(plain)) != 0) {
            fprintf(stderr, "[nbd-vram] lzo workspace initialization failed\n");
            return -1;
        }
    }
    printf("[nbd-vram] loaded liblzo2.so.2\n");
    return 0;
}

/* A zero result means store the original page instead (including codec errors).
 * The codec is fixed for the lifetime of the store, so PTEs need no codec tag. */
static int compress_page(const char *src, char *dst)
{
    if (g_compress == COMP_OFF) return 0;
    if (g_compress == COMP_LZO) {
        size_t n = sizeof(t_lzo_out);
        if (!t_lzo_work || _lzo_compress((const unsigned char *)src, COMP_PAGE,
                                         t_lzo_out, &n, t_lzo_work) != 0 || n >= COMP_PAGE)
            return 0;
        memcpy(dst, t_lzo_out, n);
        return (int)n;
    }
    if (g_compress == COMP_ZSTD) {
        size_t n = _ZSTD_compressCCtx(t_zstd_cctx, dst, COMP_PAGE - 1,
                                    src, COMP_PAGE, g_compress_level);
        return _ZSTD_isError(n) ? 0 : (int)n;
    }
    return _LZ4_compress_default(src, dst, COMP_PAGE, COMP_PAGE - 1);
}

static int decompress_page(const char *src, char *dst, uint16_t len)
{
    if (g_compress == COMP_LZO) {
        size_t n = COMP_PAGE;
        return _lzo_decompress((const unsigned char *)src, len, (unsigned char *)dst,
                               &n, NULL) == 0 && n == COMP_PAGE;
    }
    if (g_compress == COMP_ZSTD)
        return _ZSTD_decompressDCtx(t_zstd_dctx, dst, COMP_PAGE, src, len) == COMP_PAGE;
    return _LZ4_decompress_safe(src, dst, len, COMP_PAGE) == COMP_PAGE;
}

static inline void lock_page(uint64_t pg)   { pthread_rwlock_wrlock(&g_plock[pg % NPLOCK]); }
static inline void unlock_page(uint64_t pg) { pthread_rwlock_unlock(&g_plock[pg % NPLOCK]); }

/* A batch contains distinct lock stripes. Acquire them in ascending order,
 * including when page numbers wrap around NPLOCK or requests arrive out of
 * order. No worker waits for a lower stripe while holding a higher one. */
static void lock_pages(const uint64_t *pages, int n, int writing)
{
    unsigned stripes[COMP_BATCH];
    for (int i = 0; i < n; i++) {
        unsigned stripe = (unsigned)(pages[i] % NPLOCK);
        int j = i;
        while (j && stripes[j - 1] > stripe) {
            stripes[j] = stripes[j - 1];
            j--;
        }
        stripes[j] = stripe;
    }
    for (int i = 0; i < n; i++) {
        if (writing) pthread_rwlock_wrlock(&g_plock[stripes[i]]);
        else pthread_rwlock_rdlock(&g_plock[stripes[i]]);
    }
}

static inline int bm_test(const uint8_t *bm, unsigned i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bm_set(uint8_t *bm, unsigned i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bm_clr(uint8_t *bm, unsigned i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static int class_for(uint32_t n)
{
    for (int i = 0; i < NCLASS; i++)
        if (k_class_sz[i] >= n) return i;
    return NCLASS - 1;
}

static int is_same_filled(const char *p, uint8_t *fill)
{
    unsigned char v = (unsigned char)p[0];
    if ((uintptr_t)p & 7) {
        for (int i = 1; i < COMP_PAGE; i++)
            if ((unsigned char)p[i] != v) return 0;
        *fill = (uint8_t)v;
        return 1;
    }
    uint64_t splat = 0x0101010101010101ULL * v;
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (int i = 0; i < COMP_PAGE / 8; i++)
        if (q[i] != splat) return 0;
    *fill = (uint8_t)v;
    return 1;
}

/* Only slabs with free slots are on a class list. All records and bitmaps are
 * allocated at startup; these helpers run under g_alloc_lock without malloc. */
static void class_link(struct slab *s)
{
    s->prev = SLAB_NONE;
    s->next = g_class_head[s->klass];
    if (s->next != SLAB_NONE) g_slabs[s->next].prev = s->chunk;
    g_class_head[s->klass] = s->chunk;
}

static void class_unlink(struct slab *s)
{
    if (s->prev != SLAB_NONE) g_slabs[s->prev].next = s->next;
    else g_class_head[s->klass] = s->next;
    if (s->next != SLAB_NONE) g_slabs[s->next].prev = s->prev;
    s->next = s->prev = SLAB_NONE;
}

static struct slab *slab_new(uint8_t klass)
{
    if (g_free_chunk == SLAB_NONE) return NULL;
    struct slab *s = &g_slabs[g_free_chunk];
    g_free_chunk = s->next;
    g_nfree_chunks--;
    s->klass = klass;
    s->nobj  = (uint16_t)(SLAB_SZ / k_class_sz[klass]);
    s->nfree = s->nobj;
    s->hint = 0;
    memset(s->bm, 0, sizeof(s->bm));
    class_link(s);
    return s;
}

static void slab_destroy(struct slab *s)
{
    class_unlink(s);
    s->nobj = s->nfree = 0;
    s->next = g_free_chunk;
    g_free_chunk = s->chunk;
    g_nfree_chunks++;
}

static int bm_alloc_obj(struct slab *s)
{
    unsigned n = s->nobj, i = s->hint;
    for (unsigned k = 0; k < n; k++) {
        if (!bm_test(s->bm, i)) {
            bm_set(s->bm, i);
            s->hint = (i + 1 < n) ? (uint16_t)(i + 1) : 0;
            return (int)i;
        }
        if (++i == n) i = 0;
    }
    return -1;
}

/* Caller holds g_alloc_lock. Return the actual class in *klass: a larger slot
 * may be borrowed when the requested class and the unused-slab pool are empty.
 * PTEs and dedup objects must retain this class for freeing and accounting. */
static void pool_free(uint64_t off, uint8_t klass);
static uint64_t pool_alloc(uint8_t *klass)
{
    if (g_pool_retired) return UINT64_MAX;
    uint32_t chunk = g_class_head[*klass];
    struct slab *s = chunk != SLAB_NONE ? &g_slabs[chunk] : slab_new(*klass);
    if (!s) {
        for (int k = *klass + 1; k < NCLASS; k++) {
            if (g_class_head[k] == SLAB_NONE) continue;
            s = &g_slabs[g_class_head[k]];
            break;
        }
    }
    if (!s) return UINT64_MAX;
    int obj = bm_alloc_obj(s);
    if (obj < 0) return UINT64_MAX;
    if (--s->nfree == 0) class_unlink(s);
    *klass = s->klass;
    g_vram_obj_bytes += k_class_sz[*klass];
    uint64_t offset = (uint64_t)s->chunk * SLAB_SZ + (uint64_t)obj * k_class_sz[*klass];
    if (g_prepare_slot && g_prepare_slot(offset)) {
        pool_free(offset, *klass);
        return UINT64_MAX;
    }
    return offset;
}

static void pool_free(uint64_t off, uint8_t klass)
{
    if (g_pool_retired && g_retired_slot_freed) g_retired_slot_freed(off);
    uint32_t chunk = (uint32_t)(off / SLAB_SZ);
    struct slab *s = &g_slabs[chunk];
    unsigned obj = (unsigned)((off % SLAB_SZ) / k_class_sz[klass]);
    if (g_vram_obj_bytes >= k_class_sz[klass])
        g_vram_obj_bytes -= k_class_sz[klass];
    bm_clr(s->bm, obj);
    if (!s->nfree) class_link(s);
    s->nfree++;
    if (s->nfree == s->nobj)
        slab_destroy(s);
}

/* Detect accidental metadata changes; this tag is not cryptographic. */
static uint64_t disk_metadata_tag(uint64_t pg, uint64_t hash, uint8_t slot)
{
    uint64_t tag = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 8; i++) { tag ^= (pg >> (i * 8)) & 255; tag *= UINT64_C(1099511628211); }
    for (unsigned i = 0; i < 8; i++) { tag ^= (hash >> (i * 8)) & 255; tag *= UINT64_C(1099511628211); }
    tag ^= slot;
    return tag * UINT64_C(1099511628211);
}

static int valid_disk_entry(uint64_t pg, struct pte entry)
{
    return g_back_fd >= 0 && pg < g_npages && entry.kind == PTE_DISK &&
           entry.fill <= 1 && !entry.clen && !entry.klass &&
           entry.checksum == disk_metadata_tag(pg, entry.vram_off, entry.fill);
}

static uint32_t disk_read(uint64_t pg, struct pte entry, char *dst)
{
    if (!valid_disk_entry(pg, entry)) return EIO;
    ssize_t n;
    off_t offset = (off_t)((pg * 2 + entry.fill) * COMP_PAGE);
    do n = pread(g_back_fd, t_disk_page, COMP_PAGE, offset); while (n < 0 && errno == EINTR);
    /* Noncryptographic check detects common corruption, not malicious changes. */
    if (n != COMP_PAGE || page_hash(t_disk_page) != entry.vram_off) return EIO;
    memcpy(dst, t_disk_page, COMP_PAGE);
    return 0;
}

static int valid_lzo_vram_entry(struct pte e)
{
    if (e.klass >= NCLASS || (e.kind != PTE_RAW && e.kind != PTE_COMPRESSED)) return 0;
    size_t len = e.kind == PTE_RAW ? COMP_PAGE : e.clen;
    if ((e.kind == PTE_RAW && e.clen != COMP_PAGE) ||
        (e.kind == PTE_COMPRESSED && (!e.clen || e.clen >= COMP_PAGE)) ||
        len > k_class_sz[e.klass] || e.vram_off >= g_vram_size ||
        len > g_vram_size - e.vram_off) return 0;
    uint64_t within = e.vram_off % SLAB_SZ;
    return within % k_class_sz[e.klass] == 0 && len <= SLAB_SZ - within;
}

static uint32_t load_plain(uint64_t pg, char *dst, CUstream stream)
{
    struct pte e = g_ptes[pg];
    if (g_compress == COMP_LZO && (e.kind == PTE_RAW || e.kind == PTE_COMPRESSED) &&
        !valid_lzo_vram_entry(e)) return EIO;
    switch (e.kind) {
    case PTE_NONE:
        memset(dst, 0, COMP_PAGE);
        return 0;
    case PTE_SAME:
        memset(dst, e.fill, COMP_PAGE);
        break;
    case PTE_DISK:
        return disk_read(pg, e, dst);
    case PTE_RAW:
        if (_cuMemcpyDtoHAsync(dst, g_vram_ptr + e.vram_off, COMP_PAGE, stream) != CUDA_SUCCESS)
            return EIO;
        if (_cuStreamSynchronize(stream) != CUDA_SUCCESS) return EIO;
        break;
    case PTE_COMPRESSED:
        if (_cuMemcpyDtoHAsync(t_cstage, g_vram_ptr + e.vram_off, e.clen, stream) != CUDA_SUCCESS)
            return EIO;
        if (_cuStreamSynchronize(stream) != CUDA_SUCCESS) return EIO;
        if (!decompress_page(t_cstage, dst, e.clen))
            return EIO;
        break;
    default:
        return EIO;
    }
    return g_compress == COMP_LZO && page_hash(dst) != e.checksum ? EIO : 0;
}

static void prepare_plain(uint64_t pg, const char *plain, int slot, struct cpend *op)
{
    uint8_t fill;
    op->plain    = plain;
    op->hash     = (g_dedup || g_compress == COMP_LZO) ? page_hash(plain) : 0;
    op->pg       = pg;
    op->slot     = (uint8_t)slot;
    op->new_off  = 0;
    op->fill     = 0;
    op->clen     = 0;
    op->klass    = 0;
    if (is_same_filled(plain, &fill)) {
        op->kind = PTE_SAME;
        op->fill = fill;
        return;
    }
    char *slotp = t_cstage + (size_t)slot * COMP_PAGE;
    int csz = compress_page(plain, slotp);
    if (csz <= 0) {
        memcpy(slotp, plain, COMP_PAGE);
        op->kind  = PTE_RAW;
        op->clen  = COMP_PAGE;
        op->klass = (uint8_t)class_for(COMP_PAGE);
    } else {
        op->kind  = PTE_COMPRESSED;
        op->clen  = (uint16_t)csz;
        op->klass = (uint8_t)class_for((uint32_t)csz);
    }
}

static void pte_kind_add(uint8_t kind, int delta)
{
    unsigned long *p = NULL;
    switch (kind) {
    case PTE_COMPRESSED:  p = &g_pages_compressed;  break;
    case PTE_RAW:  p = &g_pages_raw;  break;
    case PTE_SAME: p = &g_pages_same; break;
    case PTE_DISK: p = &g_pages_disk; break;
    default: return;
    }
    if (delta > 0)
        __sync_fetch_and_add(p, (unsigned long)delta);
    else
        __sync_fetch_and_sub(p, (unsigned long)(-delta));
}

/* Caller holds the page lock. The other slot remains intact on a short write. */
static uint32_t disk_replace(struct cpend *op)
{
    struct pte old = g_ptes[op->pg];
    if (old.kind == PTE_DISK && !valid_disk_entry(op->pg, old)) return EIO;
    uint8_t slot = old.kind == PTE_DISK ? !old.fill : 0;
    off_t offset = (off_t)((op->pg * 2 + slot) * COMP_PAGE);
    memcpy(t_disk_page, op->plain, COMP_PAGE);
    ssize_t n;
    do n = pwrite(g_back_fd, t_disk_page, COMP_PAGE, offset); while (n < 0 && errno == EINTR);
    if (n != COMP_PAGE) return EIO;
    uint64_t hash = page_hash(t_disk_page);
    g_ptes[op->pg] = (struct pte){ .kind = PTE_DISK, .fill = slot, .vram_off = hash,
                                   .checksum = disk_metadata_tag(op->pg, hash, slot) };
    if (old.kind == PTE_RAW || old.kind == PTE_COMPRESSED) {
        pthread_mutex_lock(&g_alloc_lock);
        pool_free(old.vram_off, old.klass);
        pthread_mutex_unlock(&g_alloc_lock);
    }
    pte_kind_add(old.kind, -1);
    pte_kind_add(PTE_DISK, 1);
    return 0;
}

/* All dedup helpers below require g_dedup_lock. A reference may belong to a
 * published PTE or a pending batch. Pending references are rolled back together
 * on failure; old PTEs and their references remain intact until every new payload
 * has been copied and synchronized successfully. */
static void dedup_object_release(struct dedup_obj *obj)
{
    obj->refs = 0;
    obj->prev = NULL;
    obj->next = g_dedup_free;
    g_dedup_free = obj;
}

static void dedup_put(struct dedup_obj *obj)
{
    if (!obj) return;
    g_dedup_refs--;
    if (--obj->refs) {
        g_dedup_saved_bytes -= k_class_sz[obj->klass];
        return;
    }
    if (obj->prev) { /* Pending objects are not indexed until DMA succeeds. */
        *obj->prev = obj->next;
        if (obj->next) obj->next->prev = obj->prev;
    }
    g_dedup_unique--;
    pthread_mutex_lock(&g_alloc_lock);
    pool_free(obj->vram_off, obj->klass);
    pthread_mutex_unlock(&g_alloc_lock);
    dedup_object_release(obj);
}

static uint32_t dedup_get(struct cpend *ops, struct dedup_obj **objects, int idx,
                          unsigned *hits, int *upload, CUstream stream)
{
    struct cpend *op = &ops[idx];
    struct dedup_obj **out = &objects[idx];
    /* Earlier pages in this batch still have their original bytes in host
     * memory. Match there, avoiding a download of an as-yet-unwritten slot. */
    for (int i = 0; i < idx; i++) {
        struct dedup_obj *obj = objects[i];
        if (!obj || obj->hash != op->hash || memcmp(ops[i].plain, op->plain, COMP_PAGE))
            continue;
        obj->refs++;
        g_dedup_refs++;
        g_dedup_saved_bytes += k_class_sz[obj->klass];
        (*hits)++;
        *out = obj;
        return 0;
    }
    size_t bucket = op->hash & (g_dedup_buckets - 1);
    char *compare = t_cstage + (size_t)COMP_BATCH * COMP_PAGE;
    for (struct dedup_obj *obj = g_dedup_index[bucket]; obj; obj = obj->next) {
        if (obj->hash != op->hash) continue;
        if (g_compress == COMP_LZO && !valid_lzo_vram_entry((struct pte){
                .kind = obj->kind, .klass = obj->klass, .clen = obj->clen,
                .vram_off = obj->vram_off })) return EIO;
        CUresult r = _cuMemcpyDtoHAsync(compare, g_vram_ptr + obj->vram_off,
                                        obj->clen, stream);
        CUresult sync = _cuStreamSynchronize(stream);
        if (r != CUDA_SUCCESS || sync != CUDA_SUCCESS) return EIO;
        const char *plain = compare;
        if (obj->kind == PTE_COMPRESSED) {
            if (!decompress_page(compare, t_dedup_plain, obj->clen)) return EIO;
            plain = t_dedup_plain;
        }
        if (g_compress == COMP_LZO && page_hash(plain) != obj->hash) return EIO;
        if (memcmp(plain, op->plain, COMP_PAGE) != 0) continue;
        obj->refs++;
        g_dedup_refs++;
        g_dedup_saved_bytes += k_class_sz[obj->klass];
        (*hits)++;
        *out = obj;
        return 0;
    }

    struct dedup_obj *obj = g_dedup_free;
    if (!obj) return ENOSPC;
    pthread_mutex_lock(&g_alloc_lock);
    uint64_t off = pool_alloc(&op->klass);
    pthread_mutex_unlock(&g_alloc_lock);
    if (off == UINT64_MAX) return ENOSPC;
    g_dedup_free = obj->next;

    obj->hash = op->hash;
    obj->vram_off = off;
    obj->clen = op->clen;
    obj->kind = op->kind;
    obj->klass = op->klass;
    obj->refs = 1;
    obj->next = NULL;
    obj->prev = NULL;
    g_dedup_unique++;
    g_dedup_refs++;
    *out = obj;
    *upload = 1;
    return 0;
}

/* Page locks are already held. Reserve references first, upload all new objects
 * with one sync, then index them and publish. The dedup lock keeps other writers
 * from observing reservations and protects exact comparisons with live objects. */
static uint32_t dedup_commit(struct cpend *ops, int n, CUstream stream)
{
    struct dedup_obj *objects[COMP_BATCH] = {0};
    int upload[COMP_BATCH] = {0};
    uint32_t error = 0;
    unsigned hits = 0;
    pthread_mutex_lock(&g_dedup_lock);
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == PTE_SAME) continue;
        error = dedup_get(ops, objects, i, &hits, &upload[i], stream);
        if (error) break;
    }
    int queued = 0;
    if (!error) {
        for (int i = 0; i < n; i++) {
            if (!upload[i]) continue;
            if (_cuMemcpyHtoDAsync(g_vram_ptr + objects[i]->vram_off,
                                  t_cstage + (size_t)ops[i].slot * COMP_PAGE,
                                  ops[i].clen, stream) != CUDA_SUCCESS) {
                error = EIO;
                break;
            }
            queued++;
        }
    }
    /* Drain even after an enqueue failure before recycling any reserved slot. */
    if (queued && _cuStreamSynchronize(stream) != CUDA_SUCCESS) error = EIO;
    if (error) {
        for (int i = 0; i < n; i++) dedup_put(objects[i]);
        if (error == ENOSPC) __sync_fetch_and_add(&g_comp_enospc, 1);
    } else {
        for (int i = 0; i < n; i++) {
            if (!upload[i]) continue;
            struct dedup_obj *obj = objects[i];
            size_t bucket = obj->hash & (g_dedup_buckets - 1);
            obj->next = g_dedup_index[bucket];
            obj->prev = &g_dedup_index[bucket];
            if (obj->next) obj->next->prev = &obj->next;
            g_dedup_index[bucket] = obj;
        }
        for (int i = 0; i < n; i++) {
            struct pte *e = &g_ptes[ops[i].pg];
            struct dedup_obj *old = g_dedup_ptes[ops[i].pg];
            struct dedup_obj *obj = objects[i];
            *e = (struct pte){ .kind = PTE_SAME, .fill = ops[i].fill };
            if (g_compress == COMP_LZO) e->checksum = ops[i].hash;
            if (obj) {
                e->kind = obj->kind;
                e->clen = obj->clen;
                e->klass = obj->klass;
                e->vram_off = obj->vram_off;
            }
            g_dedup_ptes[ops[i].pg] = obj;
            pte_kind_add(ops[i].old_kind, -1);
            pte_kind_add(e->kind, 1);
            dedup_put(old);
        }
        g_dedup_hits += hits;
    }
    pthread_mutex_unlock(&g_dedup_lock);
    for (int i = 0; i < n; i++) unlock_page(ops[i].pg);
    return error;
}

static void lock_pending_writes(struct cpend *ops, int n)
{
    if (n <= 0) return;
    uint64_t pages[COMP_BATCH];
    for (int i = 0; i < n; i++) pages[i] = ops[i].pg;
    lock_pages(pages, n, 1);
}

/* Snapshot only while holding the page locks. With unlocked uploads, capture
 * after DMA: another connection may have overwritten or trimmed the page. */
static void snapshot_old_pages(struct cpend *ops, int n)
{
    for (int i = 0; i < n; i++) {
        struct pte *e = &g_ptes[ops[i].pg];
        ops[i].old_kind = e->kind;
        ops[i].old_off = e->vram_off;
        ops[i].old_klass = e->klass;
    }
}

/* Full-page, non-dedup writes enter without page locks: reserved slots are
 * private until DMA completes and the mappings are published. Partial writes
 * keep their locks throughout read/modify/write; dedup keeps its existing lock
 * order and reference lifetime. Releases any held page locks on every path. */
static uint32_t cpend_alloc_and_commit(struct cpend *ops, int n, CUstream stream,
                                     int pages_locked)
{
    if (n <= 0) return 0;
    if (g_back_fd >= 0 && pages_locked) {
        for (int i = 0; i < n; i++) {
            struct pte old = g_ptes[ops[i].pg];
            if (old.kind == PTE_DISK && !valid_disk_entry(ops[i].pg, old)) {
                for (int j = 0; j < n; j++) unlock_page(ops[j].pg);
                return EIO;
            }
        }
    }
    if (g_dedup) {
        snapshot_old_pages(ops, n);
        return dedup_commit(ops, n, stream);
    }

    pthread_mutex_lock(&g_alloc_lock);
    int failed = -1;
    uint32_t error = 0;
    int queued = 0;
    for (int i = 0; i < n; i++) {
        if (ops[i].kind != PTE_COMPRESSED && ops[i].kind != PTE_RAW)
            continue;
        uint64_t off = pool_alloc(&ops[i].klass);
        if (off == UINT64_MAX) { failed = i; break; }
        ops[i].new_off = off;
    }
    if (failed >= 0) {
        for (int i = 0; i < failed; i++) {
            if (ops[i].kind == PTE_COMPRESSED || ops[i].kind == PTE_RAW)
                pool_free(ops[i].new_off, ops[i].klass);
        }
        pthread_mutex_unlock(&g_alloc_lock);
        if (g_back_fd >= 0 && pages_locked && n == 1) {
            uint32_t result = disk_replace(&ops[0]);
            unlock_page(ops[0].pg);
            return result;
        }
        if (pages_locked)
            for (int i = 0; i < n; i++) unlock_page(ops[i].pg);
        if (__sync_fetch_and_add(&g_comp_enospc, 1) == 0)
            fprintf(stderr, "[nbd-vram] compressed VRAM pool full (ENOSPC)\n");
        return ENOSPC;
    }
    pthread_mutex_unlock(&g_alloc_lock);

    for (int i = 0; i < n; i++) {
        if (ops[i].kind != PTE_COMPRESSED && ops[i].kind != PTE_RAW)
            continue;
        size_t nbytes = (ops[i].kind == PTE_RAW) ? COMP_PAGE : ops[i].clen;
        CUresult r = _cuMemcpyHtoDAsync(g_vram_ptr + ops[i].new_off,
                                        t_cstage + (size_t)ops[i].slot * COMP_PAGE,
                                        nbytes, stream);
        if (r != CUDA_SUCCESS) {
            error = EIO;
            break;
        }
        queued++;
    }
    if (queued && _cuStreamSynchronize(stream) != CUDA_SUCCESS) error = EIO;
    if (error) {
        pthread_mutex_lock(&g_alloc_lock);
        for (int i = 0; i < n; i++) {
            if (ops[i].kind == PTE_COMPRESSED || ops[i].kind == PTE_RAW)
                pool_free(ops[i].new_off, ops[i].klass);
        }
        pthread_mutex_unlock(&g_alloc_lock);
        if (pages_locked)
            for (int i = 0; i < n; i++) unlock_page(ops[i].pg);
        return error;
    }

    /* No allocator lock is held while acquiring page locks. Read the current
     * mappings now, so an intervening write/TRIM cannot make old_off stale. */
    if (!pages_locked) lock_pending_writes(ops, n);
    snapshot_old_pages(ops, n);
    pthread_mutex_lock(&g_alloc_lock);
    for (int i = 0; i < n; i++) {
        if (ops[i].old_kind == PTE_COMPRESSED || ops[i].old_kind == PTE_RAW)
            pool_free(ops[i].old_off, ops[i].old_klass);
    }
    pthread_mutex_unlock(&g_alloc_lock);

    for (int i = 0; i < n; i++) {
        struct pte *e = &g_ptes[ops[i].pg];
        e->kind     = ops[i].kind;
        e->fill     = ops[i].fill;
        e->clen     = ops[i].clen;
        e->klass    = ops[i].klass;
        e->vram_off = ops[i].new_off;
        e->checksum = g_compress == COMP_LZO ? ops[i].hash : 0;
        if (ops[i].kind == PTE_SAME) {
            e->vram_off = 0;
            e->clen = 0;
            e->klass = 0;
        }
        pte_kind_add(ops[i].old_kind, -1);
        pte_kind_add(ops[i].kind, 1);
        unlock_page(ops[i].pg);
    }
    return 0;
}

static uint32_t write_pending(struct cpend *ops, int n, CUstream stream)
{
    if (g_back_fd >= 0) {
        for (int i = 0; i < n; i++) {
            lock_page(ops[i].pg);
            uint32_t result = cpend_alloc_and_commit(&ops[i], 1, stream, 1);
            if (result) return result;
        }
        return 0;
    }
    if (g_dedup) lock_pending_writes(ops, n);
    return cpend_alloc_and_commit(ops, n, stream, g_dedup);
}

static uint32_t store_partial(uint64_t pg, const char *frag, uint32_t lo, uint32_t hi, CUstream stream)
{
    lock_page(pg);
    uint32_t err = load_plain(pg, t_page, stream);
    if (err) { unlock_page(pg); return err; }
    memcpy(t_page + lo, frag, hi - lo);
    struct cpend op;
    prepare_plain(pg, t_page, 0, &op);
    return cpend_alloc_and_commit(&op, 1, stream, 1);
}

static uint32_t comp_write(uint64_t offset, const char *buf, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    if (!t_cstage) return EIO;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    struct cpend ops[COMP_BATCH];
    int n = 0;

    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            if (n) {
                uint32_t err = write_pending(ops, n, stream);
                n = 0;
                if (err) return err;
            }
            uint32_t err = store_partial(pg, buf + (pg_off + lo - offset), lo, hi, stream);
            if (err) return err;
            continue;
        }
        prepare_plain(pg, buf + (pg_off - offset), n, &ops[n]);
        n++;
        if (n == COMP_BATCH) {
            uint32_t err = write_pending(ops, n, stream);
            n = 0;
            if (err) return err;
        }
    }
    if (n) return write_pending(ops, n, stream);
    return 0;
}

static uint32_t rpend_commit(struct rpend *ops, int n, CUstream stream)
{
    if (n <= 0) return 0;
    if (g_back_fd >= 0 && n > 1) {
        for (int i = 0; i < n; i++) {
            uint32_t error = rpend_commit(&ops[i], 1, stream);
            if (error) return error;
        }
        return 0;
    }
    uint64_t pages[COMP_BATCH];
    for (int i = 0; i < n; i++) pages[i] = ops[i].pg;
    lock_pages(pages, n, 0);
    uint32_t error = 0;
    int queued = 0;
    for (int i = 0; i < n; i++) {
        struct pte e = g_ptes[ops[i].pg];
        ops[i].kind = e.kind;
        ops[i].clen = e.clen;
        ops[i].vram_off = e.vram_off;
        ops[i].checksum = e.checksum;
        if (e.kind == PTE_NONE || e.kind == PTE_SAME) {
            memset(ops[i].dst, e.kind == PTE_NONE ? 0 : e.fill, COMP_PAGE);
            continue;
        }
        if (e.kind == PTE_DISK) {
            error = disk_read(ops[i].pg, e, ops[i].dst);
            if (error) break;
            continue;
        }
        if (e.kind != PTE_RAW && e.kind != PTE_COMPRESSED) {
            error = EIO;
            break;
        }
        if (g_compress == COMP_LZO && !valid_lzo_vram_entry(e)) {
            error = EIO;
            break;
        }
        size_t nbytes = (ops[i].kind == PTE_RAW) ? COMP_PAGE : ops[i].clen;
        CUresult r = _cuMemcpyDtoHAsync(t_cstage + (size_t)ops[i].slot * COMP_PAGE,
                                        g_vram_ptr + ops[i].vram_off, nbytes, stream);
        if (r != CUDA_SUCCESS) {
            error = EIO;
            break;
        }
        queued++;
    }
    if (queued && _cuStreamSynchronize(stream) != CUDA_SUCCESS) error = EIO;
    /* DMA has finished using the VRAM objects. Decode only from this worker's
     * private staging and metadata snapshots, allowing overwrite/TRIM to proceed. */
    for (int i = 0; i < n; i++) unlock_page(ops[i].pg);
    for (int i = 0; i < n && !error; i++) {
        char *dst = ops[i].dst;
        char *src = t_cstage + (size_t)ops[i].slot * COMP_PAGE;
        if (ops[i].kind == PTE_RAW) {
            memcpy(dst, src, COMP_PAGE);
        } else if (ops[i].kind == PTE_COMPRESSED && !decompress_page(src, dst, ops[i].clen)) {
            error = EIO;
        }
        if (!error && g_compress == COMP_LZO && ops[i].kind != PTE_NONE &&
            ops[i].kind != PTE_DISK && page_hash(dst) != ops[i].checksum)
            error = EIO;
    }
    return error;
}

static uint32_t comp_read(uint64_t offset, char *buf, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    if (!t_cstage) return EIO;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    struct rpend ops[COMP_BATCH];
    int n = 0;

    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            if (n) {
                uint32_t err = rpend_commit(ops, n, stream);
                n = 0;
                if (err) return err;
            }
            struct rpend op = { .pg = pg, .dst = t_page, .slot = 0 };
            uint32_t err = rpend_commit(&op, 1, stream);
            if (err) return err;
            memcpy(buf + (pg_off + lo - offset), t_page + lo, hi - lo);
            continue;
        }
        ops[n].pg       = pg;
        ops[n].dst      = buf + (pg_off - offset);
        ops[n].slot     = (uint8_t)n;
        n++;
        if (n == COMP_BATCH) {
            uint32_t err = rpend_commit(ops, n, stream);
            n = 0;
            if (err) return err;
        }
    }
    if (n) return rpend_commit(ops, n, stream);
    return 0;
}

static uint32_t comp_trim(uint64_t offset, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            char z[COMP_PAGE];
            memset(z, 0, hi - lo);
            uint32_t err = store_partial(pg, z, lo, hi, stream);
            if (err) return err;
            continue;
        }
        lock_page(pg);
        if (g_dedup) pthread_mutex_lock(&g_dedup_lock);
        struct pte old = g_ptes[pg];
        if (old.kind == PTE_DISK && !valid_disk_entry(pg, old)) {
            if (g_dedup) pthread_mutex_unlock(&g_dedup_lock);
            unlock_page(pg);
            return EIO;
        }
        memset(&g_ptes[pg], 0, sizeof(g_ptes[pg]));
        pte_kind_add(old.kind, -1);
        if (g_dedup) {
            dedup_put(g_dedup_ptes[pg]);
            g_dedup_ptes[pg] = NULL;
            pthread_mutex_unlock(&g_dedup_lock);
        } else if (old.kind == PTE_COMPRESSED || old.kind == PTE_RAW) {
            pthread_mutex_lock(&g_alloc_lock);
            pool_free(old.vram_off, old.klass);
            pthread_mutex_unlock(&g_alloc_lock);
        }
        unlock_page(pg);
    }
    return 0;
}

static void compress_status_write(void)
{
    uint32_t slabs_used;
    uint64_t obj_bytes, dedup_pages, dedup_unique, dedup_saved, dedup_hits, index_bytes;
    pthread_mutex_lock(&g_dedup_lock);
    pthread_mutex_lock(&g_alloc_lock);
    slabs_used = g_nchunks - g_nfree_chunks;
    obj_bytes  = g_vram_obj_bytes;
    pthread_mutex_unlock(&g_alloc_lock);
    dedup_pages = g_dedup_refs - g_dedup_unique;
    dedup_unique = g_dedup_unique;
    dedup_saved = g_dedup_saved_bytes;
    dedup_hits = g_dedup_hits;
    index_bytes = dedup_metadata_bytes();
    unsigned long compressed = __atomic_load_n(&g_pages_compressed, __ATOMIC_RELAXED);
    unsigned long raw = __atomic_load_n(&g_pages_raw, __ATOMIC_RELAXED);
    unsigned long same = __atomic_load_n(&g_pages_same, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_dedup_lock);
    char buf[1536];
    int cfg_whole = g_compress_ratio_tenths / 10;
    int cfg_frac  = g_compress_ratio_tenths % 10;
    int n = snprintf(buf, sizeof(buf),
        "compress=%d\n"
        "dedup=%d\n"
        "dedup_pages=%llu\n"
        "dedup_unique_pages=%llu\n"
        "dedup_saved_bytes=%llu\n"
        "dedup_hits=%llu\n"
        "dedup_index_bytes=%llu\n"
        "algorithm=%s\n"
        "compression_level=%d\n"
        "configured_ratio=%d.%d\n"
        "configured_ratio_tenths=%d\n"
        "vram_bytes=%llu\n"
        "export_bytes=%llu\n"
        "vram_slab_bytes=%llu\n"
        "vram_obj_bytes=%llu\n"
        "pages_lz4=%lu\n"
        "pages_zstd=%lu\n"
        "pages_lzo=%lu\n"
        "pages_raw=%lu\n"
        "pages_same=%lu\n"
        "enospc=%lu\n",
        g_compress != COMP_OFF, g_dedup,
        (unsigned long long)dedup_pages, (unsigned long long)dedup_unique,
        (unsigned long long)dedup_saved, (unsigned long long)dedup_hits,
        (unsigned long long)index_bytes,
        compression_name(), g_compress == COMP_ZSTD ? g_compress_level : 0,
        cfg_whole, cfg_frac, g_compress_ratio_tenths,
        (unsigned long long)g_vram_size,
        (unsigned long long)g_export_size,
        (unsigned long long)slabs_used * SLAB_SZ,
        (unsigned long long)obj_bytes,
        g_compress == COMP_LZ4 ? compressed : 0,
        g_compress == COMP_ZSTD ? compressed : 0,
        g_compress == COMP_LZO ? compressed : 0, raw, same,
        __atomic_load_n(&g_comp_enospc, __ATOMIC_RELAXED));
    if (n <= 0 || n >= (int)sizeof(buf)) return;

    int fd = open(STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    if (w == (ssize_t)n)
        rename(STATUS_TMP, STATUS_PATH);
    else
        unlink(STATUS_TMP);
}

static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_start_cond = PTHREAD_COND_INITIALIZER;
static int g_start_reported, g_start_failed, g_start_release;

static int worker_protect(void)
{
    int failed = prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0;
    if (failed)
        fprintf(stderr, "[nbd-vram] worker PR_SET_IO_FLUSHER failed (%s)\n", strerror(errno));
    return failed;
}

static int worker_start(int failed)
{
    pthread_mutex_lock(&g_start_lock);
    g_start_failed |= failed;
    g_start_reported++;
    pthread_cond_broadcast(&g_start_cond);
    while (!g_start_release) pthread_cond_wait(&g_start_cond, &g_start_lock);
    failed = g_start_failed;
    pthread_mutex_unlock(&g_start_lock);
    return failed ? -1 : 0;
}

static void *status_worker(void *arg)
{
    (void)arg;
    if (worker_start(worker_protect()) != 0) return NULL;
    while (g_running) {
        compress_status_write();
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
    unlink(STATUS_PATH);
    unlink(STATUS_TMP);
    return NULL;
}

/* Fault in writable backing now, including calloc's lazily mapped zero pages.
 * Volatile stores keep this pass from being optimized back into calloc. The
 * daemon's mlockall keeps this metadata resident while serving swap requests. */
static void *metadata_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
    size_t bytes = count * size;
    void *ptr = calloc(count, size);
    if (!ptr) return NULL;
    long page_size = sysconf(_SC_PAGESIZE);
    size_t step = page_size > 0 ? (size_t)page_size : 4096;
    volatile unsigned char *p = ptr;
    for (size_t i = 0; i < bytes; i += step) p[i] = 0;
    if (bytes) p[bytes - 1] = 0;
    return ptr;
}

static int backing_init(const char *dir)
{
    /* shortcut: topology admission is a snapshot, not protection against live reconfiguration. */
    struct stat st;
    struct statfs fs;
    struct statx sx;
    int inode_flags;
    unsigned backing_sector = 0;
    if (g_dedup || !g_compress || !g_export_size || g_export_size % COMP_PAGE ||
        g_export_size > INT64_MAX / 2) {
        fprintf(stderr, "[nbd-vram] backing requires compression, no dedup, and a bounded export\n");
        return -1;
    }
    int fd = open(dir, O_TMPFILE | O_EXCL | O_RDWR | O_DIRECT | O_CLOEXEC, 0600);
    if (fd < 0) { perror("backing open"); return -1; }
    if (fstatfs(fd, &fs) != 0 || (fs.f_type != EXT4_SUPER_MAGIC && fs.f_type != XFS_SUPER_MAGIC &&
                                   fs.f_type != BTRFS_SUPER_MAGIC)) {
        fprintf(stderr, "[nbd-vram] backing requires a supported ext4, XFS or Btrfs filesystem\n");
        close(fd); return -1;
    }
    if (fs.f_type == BTRFS_SUPER_MAGIC) {
        if (backing_btrfs_topology(fd) != 0) {
            fprintf(stderr, "[nbd-vram] Btrfs backing device topology cannot be admitted\n");
            close(fd); return -1;
        }
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 0 ||
            st.st_size != 0 || st.st_blocks != 0 ||
            ioctl(fd, FS_IOC_GETFLAGS, &inode_flags) != 0 || (inode_flags & FS_COMPR_FL)) {
            fprintf(stderr, "[nbd-vram] cannot verify empty, uncompressed Btrfs backing\n");
            close(fd); return -1;
        }
        inode_flags |= FS_NOCOW_FL;
        if (ioctl(fd, FS_IOC_SETFLAGS, &inode_flags) != 0 ||
            ioctl(fd, FS_IOC_GETFLAGS, &inode_flags) != 0 ||
            !(inode_flags & FS_NOCOW_FL) || (inode_flags & FS_COMPR_FL)) {
            fprintf(stderr, "[nbd-vram] cannot enable and verify Btrfs NOCOW\n");
            close(fd); return -1;
        }
    }
    memset(&sx, 0, sizeof(sx));
    if (fs.f_type == BTRFS_SUPER_MAGIC) {
        struct btrfs_ioctl_fs_info_args info = {0};
        if (ioctl(fd, BTRFS_IOC_FS_INFO, &info) != 0 || !info.sectorsize ||
            (info.sectorsize & (info.sectorsize - 1)) || COMP_PAGE % info.sectorsize) {
            fprintf(stderr, "[nbd-vram] Btrfs sector alignment unavailable or incompatible\n");
            close(fd); return -1;
        }
        backing_sector = info.sectorsize;
    } else if (statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &sx) != 0 ||
        !(sx.stx_mask & STATX_DIOALIGN) || !sx.stx_dio_mem_align || !sx.stx_dio_offset_align ||
        COMP_PAGE % sx.stx_dio_mem_align || COMP_PAGE % sx.stx_dio_offset_align) {
        fprintf(stderr, "[nbd-vram] backing direct-I/O alignment cannot be verified\n");
        close(fd); return -1;
    }
    off_t bytes = (off_t)(2 * g_export_size);
    if (fallocate(fd, 0, 0, bytes) != 0 || fstat(fd, &st) != 0 ||
        st.st_size != bytes || (uint64_t)st.st_blocks < (uint64_t)bytes / 512 ||
        (fs.f_type == BTRFS_SUPER_MAGIC &&
         (ioctl(fd, FS_IOC_GETFLAGS, &inode_flags) != 0 ||
          !(inode_flags & FS_NOCOW_FL) || (inode_flags & FS_COMPR_FL)))) {
        fprintf(stderr, "[nbd-vram] backing preallocation failed or is incomplete\n");
        close(fd); return -1;
    }
    if (fs.f_type == BTRFS_SUPER_MAGIC) {
        /* Convert unwritten extents before serving writes under metadata pressure. */
        if ((uintptr_t)t_disk_page % backing_sector) {
            fprintf(stderr, "[nbd-vram] Btrfs prewrite buffer alignment incompatible\n");
            close(fd); return -1;
        }
        memset(t_disk_page, 0, sizeof(t_disk_page));
        for (off_t offset = 0; offset < bytes; ) {
            size_t count = (bytes - offset < COMP_PAGE) ? (size_t)(bytes - offset) : COMP_PAGE;
            ssize_t n;
            do n = pwrite(fd, t_disk_page, count, offset); while (n < 0 && errno == EINTR);
            if (n <= 0 || (size_t)n > count || (size_t)n % backing_sector) {
                fprintf(stderr, "[nbd-vram] Btrfs backing prewrite failed or is incomplete\n");
                close(fd); return -1;
            }
            offset += n;
        }
        int rc;
        do rc = fsync(fd); while (rc < 0 && errno == EINTR);
        if (rc != 0) { perror("backing fsync"); close(fd); return -1; }
        /* Empirical experimental minimum, not a universal or runtime safety guarantee. */
        const uint64_t min_available = 64ULL * 1024 * 1024;
        struct statvfs space;
        if (fstatvfs(fd, &space) != 0) {
            perror("backing fstatvfs"); close(fd); return -1;
        }
        /* Compare against ceil(min_available / f_frsize) without multiplying. */
        if (!space.f_frsize ||
            space.f_bavail < min_available / space.f_frsize +
                             (min_available % space.f_frsize != 0)) {
            fprintf(stderr, "[nbd-vram] Btrfs backing requires at least 64 MiB available after prewrite and fsync\n");
            close(fd); return -1;
        }
    }
    g_back_fd = fd;
    fprintf(stderr, "[nbd-vram] experimental backing: runtime topology and pressure safety are unverified\n");
    return 0;
}

static int compress_init(void)
{
    if (g_compress && (g_compress == COMP_ZSTD ? load_libzstd() :
                       g_compress == COMP_LZO ? load_liblzo() : load_liblz4()) != 0) return -1;
    if (g_vram_size < SLAB_SZ || (g_vram_size % SLAB_SZ) != 0) {
        fprintf(stderr, "[nbd-vram] VRAM size not aligned to %u KiB slabs\n", SLAB_SZ / 1024);
        return -1;
    }
    /* Fractional ratios can leave a partial page. Do not advertise bytes beyond
     * the last PTE; even a sector-aligned access there would overrun the table. */
    g_export_size = (g_export_size / COMP_PAGE) * COMP_PAGE;
    g_npages = g_export_size / COMP_PAGE;
    if (g_npages > SIZE_MAX / sizeof(*g_ptes) || g_vram_size / SLAB_SZ >= SLAB_NONE) {
        fprintf(stderr, "[nbd-vram] packed store metadata exceeds addressable size\n");
        return -1;
    }
    g_ptes = metadata_calloc(g_npages, sizeof(*g_ptes));
    if (!g_ptes) { perror("calloc ptes"); return -1; }
    if (g_dedup) {
        g_dedup_buckets = 16;
        while (g_dedup_buckets < g_npages / 4 && g_dedup_buckets < (1u << 20))
            g_dedup_buckets *= 2;
        /* Each logical page can hold one unique object, plus one pending batch
         * while replacing old references. Dedup commits are serialized, so no
         * per-worker multiplier is needed. Physical slot count is another cap. */
        uint64_t capacity = g_vram_size / (g_compress ? k_class_sz[0] : COMP_PAGE);
        if (capacity > g_npages + COMP_BATCH) capacity = g_npages + COMP_BATCH;
        if (capacity > SIZE_MAX / sizeof(*g_dedup_objects)) {
            fprintf(stderr, "[nbd-vram] dedup metadata exceeds addressable size\n");
            return -1;
        }
        g_dedup_capacity = (size_t)capacity;
        g_dedup_index = metadata_calloc(g_dedup_buckets, sizeof(*g_dedup_index));
        g_dedup_ptes = metadata_calloc(g_npages, sizeof(*g_dedup_ptes));
        g_dedup_objects = metadata_calloc(g_dedup_capacity, sizeof(*g_dedup_objects));
        if (!g_dedup_index || !g_dedup_ptes || !g_dedup_objects) {
            perror("calloc dedup"); return -1;
        }
        g_dedup_free = NULL;
        for (size_t i = g_dedup_capacity; i-- > 0; )
            dedup_object_release(&g_dedup_objects[i]);
    }
    g_nchunks = (uint32_t)(g_vram_size / SLAB_SZ);
    g_slabs = metadata_calloc(g_nchunks, sizeof(*g_slabs));
    if (!g_slabs) { perror("calloc slabs"); return -1; }
    for (int k = 0; k < NCLASS; k++) g_class_head[k] = SLAB_NONE;
    g_free_chunk = SLAB_NONE;
    for (uint32_t i = g_nchunks; i-- > 0; ) {
        g_slabs[i].chunk = i;
        g_slabs[i].next = g_free_chunk;
        g_free_chunk = i;
    }
    g_nfree_chunks = g_nchunks;
    for (int i = 0; i < NPLOCK; i++)
        pthread_rwlock_init(&g_plock[i], NULL);
    printf("[nbd-vram] reserved metadata: page table %.1f MiB, slabs %.1f MiB, dedup %.1f MiB\n",
           (double)g_npages * sizeof(*g_ptes) / (1024.0 * 1024.0),
           (double)g_nchunks * sizeof(*g_slabs) / (1024.0 * 1024.0),
           (double)dedup_metadata_bytes() / (1024.0 * 1024.0));
    return 0;
}

static void compress_shutdown(void)
{
    if (g_back_fd >= 0) { close(g_back_fd); g_back_fd = -1; }
    unlink(STATUS_PATH);
    unlink(STATUS_TMP);
    if (g_ptes)
        printf("[nbd-vram] compress: %u/%u slabs used, %lu ENOSPC writes\n",
               g_nchunks - g_nfree_chunks, g_nchunks, g_comp_enospc);
    /* Workers are stopped. Pools can be released even after partial init or
     * with live objects; individual records never own separate allocations. */
    free(g_dedup_index); g_dedup_index = NULL;
    free(g_dedup_ptes); g_dedup_ptes = NULL;
    free(g_dedup_objects); g_dedup_objects = g_dedup_free = NULL;
    g_dedup_capacity = 0;
    free(g_ptes); g_ptes = NULL;
    free(g_slabs); g_slabs = NULL;
    if (g_liblz4) { dlclose(g_liblz4); g_liblz4 = NULL; }
    for (int i = 0; i < g_nbd_threads; i++) {
        free(g_lzo_work[i]); g_lzo_work[i] = NULL;
        if (g_zstd_cctx[i]) _ZSTD_freeCCtx(g_zstd_cctx[i]);
        if (g_zstd_dctx[i]) _ZSTD_freeDCtx(g_zstd_dctx[i]);
        g_zstd_cctx[i] = NULL;
        g_zstd_dctx[i] = NULL;
    }
    if (g_libzstd) { dlclose(g_libzstd); g_libzstd = NULL; }
    if (g_liblzo) { dlclose(g_liblzo); g_liblzo = NULL; }
}

/* Leave incomplete frames in the socket so they cannot delay earlier replies.
 * Only this worker reads the connection: once a complete WRITE is queued,
 * consuming its header and payload cannot wait for the peer to send more. */
static int peek_request_nb(int fd, struct nbd_req_hdr *h)
{
    ssize_t r;
    do { r = recv(fd, h, sizeof(*h), MSG_PEEK | MSG_DONTWAIT); }
    while (r < 0 && errno == EINTR);
    if (r == 0) return -1;                                   /* peer closed */
    if (r < 0)  return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if ((size_t)r < sizeof(*h)) return 0;
    uint32_t len = ntohl(h->len);
    if (ntohs(h->type) == NBD_CMD_WRITE && len <= BATCH_SLOT) {
        int queued;
        int rc;
        do { rc = ioctl(fd, FIONREAD, &queued); } while (rc < 0 && errno == EINTR);
        if (rc < 0) return -1;
        if ((size_t)queued < sizeof(*h) + len) return 0;
    }
    return 1;
}

/* Per-request path: handles a single header that
 * was already read. Used for oversized requests, FLUSH/TRIM, and VRAM_BATCH=0.
 * Returns 0 to keep serving, -1 to drop the connection. */
static uint32_t read_chunk(uint64_t offset, char *buf, uint32_t len, CUstream stream)
{
    if (packed_store_enabled()) return comp_read(offset, buf, len, stream);
    CUresult r = _cuMemcpyDtoHAsync(buf, g_vram_ptr + offset, len, stream);
    if (r != CUDA_SUCCESS || _cuStreamSynchronize(stream) != CUDA_SUCCESS) return EIO;
    return 0;
}

static int handle_one(int fd, const struct nbd_req_hdr *h, CUstream stream, char *iobuf)
{
    uint16_t cmd    = ntohs(h->type);
    uint64_t handle = h->handle;
    uint64_t offset = be64toh(h->from);
    uint32_t length = ntohl(h->len);
    uint32_t error  = h->flags ? EINVAL : 0;
    uint32_t first_read = 0;

    if (g_batch_debug && (cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE))
        __sync_fetch_and_add(&g_legacy_ops, 1);

    if ((cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE) && oob(offset, length)) {
        fprintf(stderr, "[nbd-vram] oob off=%llu len=%u\n",
                (unsigned long long)offset, length);
        error = EINVAL;
    }

    if (cmd == NBD_CMD_WRITE) {
        uint32_t remaining = length;
        uint64_t voff      = offset;
        while (remaining > 0) {
            uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
            if (recv_all(fd, iobuf, chunk) != 0) return -1;
            if (!error) {
                if (packed_store_enabled()) {
                    uint32_t e = comp_write(voff, iobuf, chunk, stream);
                    if (e) error = e;
                } else {
                    CUresult r = _cuMemcpyHtoDAsync(g_vram_ptr + voff, iobuf, chunk, stream);
                    /* The next recv overwrites this pinned buffer. Finish DMA
                     * before reusing it, and keep large requests in small bursts. */
                    CUresult sync_r = _cuStreamSynchronize(stream);
                    if (r != CUDA_SUCCESS || sync_r != CUDA_SUCCESS) {
                        if (r == CUDA_SUCCESS) r = sync_r;
                        fprintf(stderr, "[nbd-vram] HtoD failed: %s\n", cuda_err(r));
                        error = EIO;
                    }
                }
                voff += chunk;
            }
            remaining -= chunk;
        }
    } else if (cmd == NBD_CMD_TRIM && !error) {
        if (oob(offset, length)) error = EINVAL;
        else if (packed_store_enabled()) error = comp_trim(offset, length, stream);
    } else if (cmd != NBD_CMD_READ && cmd != NBD_CMD_TRIM) {
        error = EINVAL;
    }
    if (cmd == NBD_CMD_READ && !error && length) {
        first_read = length > IO_BUF_SIZE ? IO_BUF_SIZE : length;
        error = read_chunk(offset, iobuf, first_read, stream);
    }

    struct nbd_resp_hdr resp;
    resp.magic  = htonl(NBD_RESPONSE_MAGIC);
    resp.error  = htonl(error);
    resp.handle = handle;
    if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
    if (error == EIO) return -1;   /* real copy failure: hard-reset the connection */
    if (error)        return 0;    /* EINVAL / ENOSPC: reported, keep serving */

    if (cmd == NBD_CMD_READ) {
        uint32_t remaining = length - first_read;
        uint64_t voff      = offset + first_read;
        if (first_read && send_all(fd, iobuf, first_read) != 0) return -1;
        while (remaining > 0) {
            uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
            if (read_chunk(voff, iobuf, chunk, stream)) return -1;
            if (send_all(fd, iobuf, chunk) != 0) return -1;
            remaining -= chunk;
            voff      += chunk;
        }
    }
    return 0;
}

/* Validate a header and, if it is a batchable READ/WRITE that fits a slot,
 * populate *op (reading the WRITE payload into the slot now, since it must be
 * drained in frame order). Returns 1 = batched, 0 = not batchable (caller falls
 * back to handle_one), -1 = protocol/socket error. */
static int batch_admit(int fd, const struct nbd_req_hdr *h, struct bop *op, char *slot)
{
    if (ntohl(h->magic) != NBD_REQUEST_MAGIC) {
        fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(h->magic));
        return -1;
    }
    uint16_t cmd = ntohs(h->type);
    uint32_t len = ntohl(h->len);
    if (h->flags || (cmd != NBD_CMD_READ && cmd != NBD_CMD_WRITE) || len == 0 || len > BATCH_SLOT)
        return 0;   /* DISC/FLUSH/TRIM or oversized: not for the batch path */

    op->handle = h->handle;
    op->offset = be64toh(h->from);
    op->len    = len;
    op->cmd    = cmd;
    op->slot   = slot;
    op->error  = oob(op->offset, len) ? EINVAL : 0;

    if (cmd == NBD_CMD_WRITE) {
        /* Drain the payload even when OOB, to stay frame-aligned; the copy is
         * skipped in flush_batch for errored ops. */
        if (recv_all(fd, slot, len) != 0) return -1;
    }
    return 1;
}

/* Group whole, aligned requests of one direction into at most COMP_BATCH pages.
 * Repeated stripes (including overlapping pages) end the group, so earlier
 * writes publish before later reads/writes are prepared. Partial requests use
 * the read-modify-write path between groups. Staging and metadata stay bounded
 * independently of VRAM_BATCH_DEPTH and the number of connections. */
static void flush_packed_ops(struct bop *ops, int n, CUstream stream)
{
    for (int first = 0; first < n; ) {
        struct bop *op = &ops[first];
        if (op->error) { first++; continue; }
        if (!t_cstage) { op->error = EIO; first++; continue; }
        if (g_back_fd >= 0 || op->offset % COMP_PAGE || op->len % COMP_PAGE) {
            op->error = op->cmd == NBD_CMD_WRITE
                ? comp_write(op->offset, op->slot, op->len, stream)
                : comp_read(op->offset, op->slot, op->len, stream);
            first++;
            continue;
        }

        uint8_t stripes[(NPLOCK + 7) / 8] = {0};
        int end = first, pages = 0;
        while (end < n) {
            struct bop *next = &ops[end];
            int count = (int)(next->len / COMP_PAGE);
            if (next->error || next->cmd != op->cmd || next->offset % COMP_PAGE ||
                next->len % COMP_PAGE || pages + count > COMP_BATCH) break;
            uint64_t pg = next->offset / COMP_PAGE;
            int conflict = 0;
            for (int j = 0; j < count; j++)
                if (bm_test(stripes, (unsigned)((pg + j) % NPLOCK))) conflict = 1;
            if (conflict) break;
            for (int j = 0; j < count; j++) bm_set(stripes, (unsigned)((pg + j) % NPLOCK));
            pages += count;
            end++;
        }

        struct cpend writes[COMP_BATCH];
        struct rpend reads[COMP_BATCH];
        int slot = 0;
        for (int i = first; i < end; i++) {
            for (uint32_t off = 0; off < ops[i].len; off += COMP_PAGE, slot++) {
                uint64_t pg = (ops[i].offset + off) / COMP_PAGE;
                if (op->cmd == NBD_CMD_WRITE)
                    prepare_plain(pg, ops[i].slot + off, slot, &writes[slot]);
                else
                    reads[slot] = (struct rpend){ .pg = pg, .dst = ops[i].slot + off,
                                                  .slot = (uint8_t)slot };
            }
        }
        uint32_t error = op->cmd == NBD_CMD_WRITE
            ? write_pending(writes, pages, stream) : rpend_commit(reads, pages, stream);
        for (int i = first; i < end; i++) ops[i].error = error;
        first = end;
    }
}

/* Direct copies share one synchronize; packed requests use bounded page groups.
 * All successful writes are published before their replies are sent.
 * NBD matches replies by handle, so a per-op error never strands the others: a bad
 * request (EINVAL) is reported and we keep serving; a real copy failure (EIO) is
 * reported too, then the connection is dropped after every reply is sent. */
static int flush_batch(int fd, struct bop *ops, int n, CUstream stream)
{
    int hard_err = 0;   /* an EIO occurred: reset the connection once all replies are out */
    if (packed_store_enabled()) {
        flush_packed_ops(ops, n, stream);
    } else {
        for (int i = 0; i < n; i++) {
            if (ops[i].error) continue;
            CUdeviceptr d = g_vram_ptr + ops[i].offset;
            CUresult r = (ops[i].cmd == NBD_CMD_READ)
                ? _cuMemcpyDtoHAsync(ops[i].slot, d, ops[i].len, stream)
                : _cuMemcpyHtoDAsync(d, ops[i].slot, ops[i].len, stream);
            if (r != CUDA_SUCCESS) {
                fprintf(stderr, "[nbd-vram] batch copy failed: %s\n", cuda_err(r));
                ops[i].error = EIO;
            }
        }
        if (_cuStreamSynchronize(stream) != CUDA_SUCCESS) {
            for (int i = 0; i < n; i++) if (!ops[i].error) ops[i].error = EIO;
        }
    }

    if (g_batch_debug) {            /* opt-in: measure real-world batch depth */
        __sync_fetch_and_add(&g_flush_count, 1);
        __sync_fetch_and_add(&g_flush_ops, n);
        if (n > 1) { __sync_fetch_and_add(&g_batch_count, 1);
                     __sync_fetch_and_add(&g_batch_ops, n); }
    }

    for (int i = 0; i < n; i++) {
        if (ops[i].error == EIO) hard_err = 1;
        struct nbd_resp_hdr resp;
        resp.magic  = htonl(NBD_RESPONSE_MAGIC);
        resp.error  = htonl(ops[i].error);
        resp.handle = ops[i].handle;
        if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
        if (ops[i].cmd == NBD_CMD_READ && !ops[i].error) {
            if (send_all(fd, ops[i].slot, ops[i].len) != 0) return -1;
        }
    }
    return hard_err ? -1 : 0;   /* EIO: every reply sent, now reset the connection */
}

static int handle_client(int fd, CUstream stream, char *batchbuf, char *iobuf)
{
    if (nbd_handshake(fd, g_export_size, packed_store_enabled()) != 0) {
        fprintf(stderr, "[nbd-vram] handshake failed\n");
        return -1;
    }
    printf("[nbd-vram] handshake OK, entering transmission mode\n");

    struct bop ops[BATCH_DEPTH_MAX];
    int depth = g_batch_depth;

    while (g_running) {
        /* Block here for the first request: this is the worker's idle wait. */
        struct nbd_req_hdr h;
        if (recv_all(fd, &h, sizeof(h)) != 0) return -1;
        if (ntohl(h.magic) != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(h.magic));
            return -1;
        }
        if (ntohs(h.type) == NBD_CMD_DISC && !h.flags) break;

        if (!g_batch_enabled || !batchbuf) {
            if (handle_one(fd, &h, stream, iobuf) != 0) return -1;
            continue;
        }

        int adm = batch_admit(fd, &h, &ops[0], batchbuf);
        if (adm < 0) return -1;
        if (adm == 0) {                       /* not batchable: legacy path */
            if (handle_one(fd, &h, stream, iobuf) != 0) return -1;
            continue;
        }

        /* Only collect complete frames already queued. A byte cap bounds GPU
         * work even when depth is raised; direction changes end the batch so
         * reads get replies before any later writes are submitted. */
        int n = 1;
        size_t bytes = ops[0].len;
        struct nbd_req_hdr extra;
        while (n < depth && bytes < BATCH_BYTES_MAX) {
            int g = peek_request_nb(fd, &extra);
            if (g <= 0) break;  /* reply to earlier work before EOF/error too */
            uint32_t len = ntohl(extra.len);
            if (ntohl(extra.magic) != NBD_REQUEST_MAGIC || extra.flags ||
                ntohs(extra.type) != ops[0].cmd || !len || len > BATCH_SLOT ||
                len > BATCH_BYTES_MAX - bytes) break;
            if (recv_all(fd, &extra, sizeof(extra)) != 0) return -1;
            if (batch_admit(fd, &extra, &ops[n], batchbuf + bytes) != 1) return -1;
            bytes += len;
            n++;
        }

        if (flush_batch(fd, ops, n, stream) != 0) return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Thread worker - one per NBD connection slot
 * ---------------------------------------------------------------------- */

static void *thread_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    CUstream stream = NULL;
    void *iobuf = NULL;
    void *batchbuf = NULL;
    int failed = worker_protect();
    CUresult r;
    if (failed) goto startup_done;
    r = _cuCtxSetCurrent(g_cu_ctx);
    if (r != CUDA_SUCCESS) {
        fprintf(stderr, "[nbd-vram] worker cuCtxSetCurrent failed: %s\n", cuda_err(r));
        failed = 1; goto startup_done;
    }
    r = _cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
    if (r != CUDA_SUCCESS || !stream) {
        fprintf(stderr, "[nbd-vram] worker cuStreamCreate failed: %s\n", cuda_err(r));
        failed = 1; goto startup_done;
    }
    /* Pinned host memory: eliminates CUDA driver staging copy for each DMA.
     * iobuf serves the per-op path; batchbuf packs variable-length requests
     * within the byte cap. Both pinned, allocated once per worker. */
    r = _cuMemAllocHost(&iobuf, IO_BUF_SIZE);
    if (r != CUDA_SUCCESS || !iobuf) {
        fprintf(stderr, "[nbd-vram] worker I/O buffer allocation failed: %s\n", cuda_err(r));
        failed = 1; goto startup_done;
    }
    if (g_batch_enabled) {
        r = _cuMemAllocHost(&batchbuf, BATCH_BYTES_MAX);
        if (r != CUDA_SUCCESS || !batchbuf) {
            fprintf(stderr, "[nbd-vram] worker batch buffer allocation failed: %s\n", cuda_err(r));
            failed = 1; goto startup_done;
        }
    }
    t_cstage = NULL;
    t_cstage_cuda = 0;
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
    t_lzo_work = g_lzo_work[idx];
    if (g_compress == COMP_LZO) memset(t_lzo_out, 0, sizeof(t_lzo_out));
    if (packed_store_enabled()) {
        if (_cuMemAllocHost((void **)&t_cstage, compression_stage_size()) == CUDA_SUCCESS && t_cstage) {
            t_cstage_cuda = 1;
        } else {
            t_cstage = calloc(1, compression_stage_size());
            if (!t_cstage) {
                fprintf(stderr, "[nbd-vram] compress staging alloc failed\n");
                failed = 1;
            }
        }
    }

startup_done:
    if (worker_start(failed) != 0) goto cleanup;
    while (g_running) {
        /* Draining (SIGTERM arrived with a client attached): take no new
         * connections, just wait for the remaining clients to detach. The
         * thread that sees the last one go flips g_running for everyone. */
        if (atomic_load(&g_term_requested)) {
            if (finish_drain_if_idle()) {
                printf("[nbd-vram] drain complete - last client gone, exiting\n");
                break;
            }
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
            continue;
        }
        /* Wait for a connection with a 1s timeout so the loop re-checks
         * g_running and exits promptly on SIGTERM (a bare blocking accept()
         * cannot be woken by the signal handler, which hung shutdown for 90s). */
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno != EINTR) worker_fatal_error("poll", errno);
            continue;
        }
        if (!pr) continue;
        pthread_mutex_lock(&g_client_lock);
        if (atomic_load(&g_term_requested)) {
            pthread_mutex_unlock(&g_client_lock);
            continue;
        }
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            int accept_error = errno;
            pthread_mutex_unlock(&g_client_lock);
            if (accept_error != EINTR && accept_error != EAGAIN && accept_error != EWOULDBLOCK)
                worker_fatal_error("accept", accept_error);
            continue;
        }
        g_client_fds[idx] = cfd;
        pthread_mutex_unlock(&g_client_lock);
        printf("[nbd-vram] client connected\n");
        int client_result = handle_client(cfd, stream, batchbuf, iobuf);
        pthread_mutex_lock(&g_client_lock);
        retire_client_locked(idx, client_result);
        close(cfd);
        pthread_mutex_unlock(&g_client_lock);
        printf("[nbd-vram] client disconnected\n");
    }

cleanup:
    if (t_cstage) {
        if (t_cstage_cuda) _cuMemFreeHost(t_cstage);
        else               free(t_cstage);
        t_cstage = NULL;
    }
    if (batchbuf) _cuMemFreeHost(batchbuf);
    if (iobuf)    _cuMemFreeHost(iobuf);
    if (stream) _cuStreamDestroy(stream);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
    CUdevice  cu_dev;
    int       ret    = 1;
    int       socket_owned = 0;
    /* Startup/tuning diagnostics must reach the journal while we are running. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *sched = getenv("VRAM_CUDA_SCHED");
    unsigned int cuda_sched = CU_CTX_SCHED_BLOCKING_SYNC;
    if (sched && strcmp(sched, "auto") == 0) cuda_sched = CU_CTX_SCHED_AUTO;
    else if (sched && strcmp(sched, "blocking") != 0) {
        fprintf(stderr, "[nbd-vram] invalid VRAM_CUDA_SCHED='%s' (expected blocking or auto)\n", sched);
        return 1;
    }

    const char *cenv = getenv("VRAM_COMPRESS");
    if (parse_compression(cenv, &g_compress, &g_compress_level) != 0) {
        fprintf(stderr, "[nbd-vram] invalid VRAM_COMPRESS='%s' (expected off, lz4, lzo, zstd, or zstd:LEVEL with LEVEL 1..22)\n", cenv);
        return 1;
    }

    const char *denv = getenv("VRAM_DEDUP");
    if (denv && strcmp(denv, "0") != 0 && strcmp(denv, "1") != 0) {
        fprintf(stderr, "[nbd-vram] invalid VRAM_DEDUP='%s' (expected 0 or 1)\n", denv);
        return 1;
    }
    g_dedup = denv && strcmp(denv, "1") == 0;
    const char *back_dir = getenv("VRAM_BACKING_DIR");
    if (back_dir && (!*back_dir || !g_compress || g_dedup || getenv("VRAM_NO_MLOCK"))) {
        fprintf(stderr, "[nbd-vram] backing requires compression, locked memory and dedup off\n");
        return 1;
    }

    /* Socket path is fixed in production; VRAM_SOCK_PATH overrides it for
     * non-root testing. Resolved early so every cleanup path sees it. */
    const char *sock_path = getenv("VRAM_SOCK_PATH");
    if (!sock_path || !*sock_path) sock_path = SOCK_PATH;

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Prevent kernel from paging out our own pages - doing so would route
     * the page fault back through this daemon, deadlocking under swap pressure.
     * Requires LimitMEMLOCK=infinity in the systemd service. VRAM_NO_MLOCK=1
     * skips it for unprivileged dev runs (MCL_FUTURE otherwise makes CUDA's huge
     * VA reservation exceed a normal user's memlock limit and cuInit OOMs). */
    if (getenv("VRAM_NO_MLOCK")) {
        fprintf(stderr, "[nbd-vram] VRAM_NO_MLOCK set - skipping mlockall (dev/test only)\n");
    } else if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[nbd-vram] mlockall failed (%s) - daemon pages may be swapped, risking deadlock\n",
                strerror(errno));
        return 1;
    }

    /* Mark the daemon as part of the I/O flush path. Sets PF_MEMALLOC_NOIO +
     * PF_LOCAL_THROTTLE so allocations made while servicing a swap write do not
     * recurse back into reclaim/writeback - that recursion is the swap-over-NBD
     * deadlock at zero free RAM. Each worker must also set it. */
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "[nbd-vram] PR_SET_IO_FLUSHER failed (%s) - deadlock risk under swap pressure\n",
                strerror(errno));
        return 1;
    }

    if (load_libcuda() != 0) goto out;

    for (int i = 0; i < 10; i++) {
        CUresult r = _cuInit(0);
        if (r == CUDA_SUCCESS) break;
        if (i == 9) {
            fprintf(stderr, "[nbd-vram] cuInit failed: %s\n", cuda_err(r));
            goto out;
        }
        fprintf(stderr, "[nbd-vram] cuInit attempt %d failed, retrying\n", i + 1);
        sleep(2);
    }

    if (_cuDeviceGet(&cu_dev, 0) != CUDA_SUCCESS) goto out;
    if (_cuCtxCreate(&g_cu_ctx, cuda_sched, cu_dev) != CUDA_SUCCESS) goto out;
    printf("[nbd-vram] CUDA waits: %s\n",
           cuda_sched == CU_CTX_SCHED_BLOCKING_SYNC ? "blocking" : "auto");

    const char *env = getenv("VRAM_SETUP_SIZE_MB");
    size_t mb = env ? (size_t)atol(env) : DEFAULT_SIZE_MB;

    {
        const char *tenv = getenv("VRAM_NBD_THREADS");
        if (tenv) {
            g_nbd_threads = atoi(tenv);
            if (g_nbd_threads < 1) g_nbd_threads = 1;
            if (g_nbd_threads > NBD_THREADS_MAX) g_nbd_threads = NBD_THREADS_MAX;
        }
        const char *benv = getenv("VRAM_BATCH");
        if (benv) g_batch_enabled = atoi(benv) != 0;
        const char *bdenv = getenv("VRAM_BATCH_DEPTH");
        if (bdenv) {
            g_batch_depth = atoi(bdenv);
            if (g_batch_depth < 1) g_batch_depth = 1;
            if (g_batch_depth > BATCH_DEPTH_MAX) g_batch_depth = BATCH_DEPTH_MAX;
        }
        const char *bgenv = getenv("VRAM_BATCH_DEBUG");
        if (bgenv) g_batch_debug = atoi(bgenv) != 0;
        const char *renv = getenv("VRAM_COMPRESS_RATIO");
        if (renv && parse_ratio_tenths(renv, &g_compress_ratio_tenths) != 0) {
            fprintf(stderr, "[nbd-vram] invalid VRAM_COMPRESS_RATIO='%s' (expected 1.0..8.0, one decimal)\n", renv);
            goto out_cuda;
        }
    }

    /* Back off 512 MiB at a time if the GPU is short on memory (e.g. display compositor loaded) */
    g_vram_ptr = 0;
    while (mb >= 1024) {
        g_vram_size = (mb * 1024ULL * 1024ULL / SIZE_ALIGN) * SIZE_ALIGN;
        printf("[nbd-vram] allocating %llu MiB of VRAM\n",
               (unsigned long long)(g_vram_size >> 20));
        CUresult alloc_r = _cuMemAlloc(&g_vram_ptr, g_vram_size);
        if (alloc_r == CUDA_SUCCESS) break;
        fprintf(stderr, "[nbd-vram] %llu MiB failed (%s), backing off 512 MiB\n",
                (unsigned long long)mb, cuda_err(alloc_r));
        g_vram_ptr = 0;
        mb -= 512;
    }
    if (!g_vram_ptr) {
        fprintf(stderr, "[nbd-vram] all allocation attempts failed\n");
        goto out_cuda;
    }
    printf("[nbd-vram] VRAM at CUDA VA 0x%llx\n", (unsigned long long)g_vram_ptr);

    g_export_size = g_vram_size;
    if (packed_store_enabled()) {
        g_export_size = (g_vram_size * (uint64_t)g_compress_ratio_tenths) / 10ULL;
        if (compress_init() != 0) goto out_cuda;
        if (back_dir && backing_init(back_dir) != 0) goto out_cuda;
        printf("[nbd-vram] compression: %s, dedup: %s (ratio %.1fx, %llu MiB VRAM advertised as %llu MiB swap, %llu pages)\n",
               compression_name(), g_dedup ? "on" : "off", (double)g_compress_ratio_tenths / 10.0,
               (unsigned long long)(g_vram_size >> 20),
               (unsigned long long)(g_export_size >> 20),
               (unsigned long long)g_npages);
    } else {
        printf("[nbd-vram] compression: off (1:1 VRAM mapping, %llu MiB)\n",
               (unsigned long long)(g_vram_size >> 20));
    }

    /* Create Unix socket (path resolved at top of main; VRAM_SOCK_PATH may
     * override it for non-root testing). */
    struct stat socket_stat;
    if (lstat(sock_path, &socket_stat) == 0 || errno != ENOENT) {
        fprintf(stderr, "[nbd-vram] socket path already exists or cannot be checked: %s\n", sock_path);
        goto out_cuda;
    }
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); goto out_cuda; }

    {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            { perror("bind"); goto out_cuda; }
        socket_owned = 1;
    }
    if (chmod(sock_path, 0600) != 0) { perror("chmod socket"); goto out_cuda; }
    if (listen(g_listen_fd, g_nbd_threads + 1) < 0) { perror("listen"); goto out_cuda; }
    /* non-blocking so the poll()+accept() in each worker never blocks if another
     * worker grabbed the pending connection first */
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);

    {
        pthread_t threads[NBD_THREADS_MAX];
        pthread_t st;
        int have_st = 0, created = 0, err = 0;
        for (int i = 0; i < g_nbd_threads; i++) g_client_fds[i] = -1;
        if (packed_store_enabled()) {
            err = pthread_create(&st, NULL, status_worker, NULL);
            have_st = err == 0;
        }
        while (!err && created < g_nbd_threads) {
            err = pthread_create(&threads[created], NULL, thread_worker, (void *)(intptr_t)created);
            if (!err) created++;
        }
        pthread_mutex_lock(&g_start_lock);
        while (g_start_reported < created + have_st)
            pthread_cond_wait(&g_start_cond, &g_start_lock);
        if (err) {
            fprintf(stderr, "[nbd-vram] pthread_create failed (%s)\n", strerror(err));
            g_start_failed = 1;
        }
        int failed = g_start_failed;
        g_start_release = 1;
        pthread_cond_broadcast(&g_start_cond);
        pthread_mutex_unlock(&g_start_lock);
        if (failed) {
            for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
            if (have_st) pthread_join(st, NULL);
            goto out_cuda;
        }

    printf("[nbd-vram] listening on %s (%d threads)\n", sock_path, g_nbd_threads);
    printf("[nbd-vram] request batching %s (depth %d, max %d KiB total); I/O chunks %d KiB\n",
           g_batch_enabled ? "on" : "off", g_batch_depth, BATCH_BYTES_MAX / 1024,
           IO_BUF_SIZE / 1024);

    /* sd_notify READY=1 */
    {
        const char *ns = getenv("NOTIFY_SOCKET");
        if (ns) {
            int nfd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (nfd >= 0) {
                struct sockaddr_un na = { .sun_family = AF_UNIX };
                const char *p = (ns[0] == '@') ? ns + 1 : ns;
                strncpy(na.sun_path, p, sizeof(na.sun_path) - 1);
                const char *msg = "READY=1\n";
                sendto(nfd, msg, strlen(msg), 0, (struct sockaddr *)&na, sizeof(na));
                close(nfd);
            }
        }
    }

        for (int i = 0; i < g_nbd_threads; i++)
            pthread_join(threads[i], NULL);
        g_running = 0;
        if (have_st) pthread_join(st, NULL);
    }
    ret = atomic_load(&g_worker_error) ? 1 : 0;

out_cuda:
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (socket_owned) unlink(sock_path);
    if (packed_store_enabled()) compress_shutdown();
    if (g_vram_ptr) _cuMemFree(g_vram_ptr);
    if (g_cu_ctx)   _cuCtxDestroy(g_cu_ctx);
    if (g_libcuda)  dlclose(g_libcuda);
out:
    if (g_batch_debug) {
        unsigned long batched_path = g_flush_ops;
        unsigned long total = batched_path + g_legacy_ops;
        printf("[nbd-vram] batched-path: %lu flushes, %lu ops, true avg depth %.2f\n",
               g_flush_count, g_flush_ops,
               g_flush_count ? (double)g_flush_ops / (double)g_flush_count : 0.0);
        printf("[nbd-vram]   of those, %lu coalesced (n>1) carrying %lu ops (avg %.1f)\n",
               g_batch_count, g_batch_ops,
               g_batch_count ? (double)g_batch_ops / (double)g_batch_count : 0.0);
        printf("[nbd-vram]   legacy/oversized ops: %lu  (%.1f%% of %lu total R/W requests)\n",
               g_legacy_ops, total ? 100.0 * (double)g_legacy_ops / (double)total : 0.0, total);
    }
    printf("[nbd-vram] exiting (ret=%d)\n", ret);
    return ret;
}
