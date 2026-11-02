#define _GNU_SOURCE
#include "test-cuda-bridge-protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef CUresult (*fn_init)(unsigned int);
typedef CUresult (*fn_device_get)(CUdevice *, int);
typedef CUresult (*fn_ctx_create)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*fn_ctx_set)(CUcontext);
typedef CUresult (*fn_ctx_destroy)(CUcontext);
typedef CUresult (*fn_mem_alloc)(CUdeviceptr *, size_t);
typedef CUresult (*fn_mem_free)(CUdeviceptr);
typedef CUresult (*fn_mem_get_info)(size_t *, size_t *);
typedef CUresult (*fn_host_alloc)(void **, size_t);
typedef CUresult (*fn_host_free)(void *);
typedef CUresult (*fn_stream_create)(CUstream *, unsigned int);
typedef CUresult (*fn_stream_sync)(CUstream);
typedef CUresult (*fn_stream_destroy)(CUstream);
typedef CUresult (*fn_htod)(CUdeviceptr, const void *, size_t, CUstream);
typedef CUresult (*fn_dtoh)(void *, CUdeviceptr, size_t, CUstream);

struct driver {
    void *handle;
    fn_init init;
    fn_device_get device_get;
    fn_ctx_create ctx_create;
    fn_ctx_set ctx_set;
    fn_ctx_destroy ctx_destroy;
    fn_mem_alloc mem_alloc;
    fn_mem_free mem_free;
    fn_mem_get_info mem_get_info;
    fn_host_alloc host_alloc;
    fn_host_free host_free;
    fn_stream_create stream_create;
    fn_stream_sync stream_sync;
    fn_stream_destroy stream_destroy;
    fn_htod htod;
    fn_dtoh dtoh;
};

struct server {
    struct driver d;
    CUcontext context;
    CUdevice device;
    CUdeviceptr allocation;
    void *stage;
    size_t allocation_size;
    size_t allocation_max;
    int initialized;
    int device_valid;
    int context_created;
    CUstream streams[16];
    uint64_t seq;
    unsigned int copies;
};

enum { CUDA_SUCCESS = 0, CUDA_ERROR_INVALID_VALUE = 1,
       CUDA_ERROR_OUT_OF_MEMORY = 2, CUDA_ERROR_INVALID_CONTEXT = 201,
       CUDA_ERROR_INVALID_HANDLE = 400, CUDA_ERROR_NOT_INITIALIZED = 3,
       CUDA_ERROR_INVALID_DEVICE = 101 };

#define LOAD(field, symbol) do { \
    *(void **)(&s->d.field) = dlsym(s->d.handle, symbol); \
    if (!s->d.field) { fprintf(stderr, "dlsym(%s): %s\n", symbol, dlerror()); return -1; } \
} while (0)

static int load_driver(struct server *s, const char *path)
{
    s->d.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s->d.handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return -1; }
    LOAD(init, "cuInit");
    LOAD(device_get, "cuDeviceGet");
    LOAD(ctx_create, "cuCtxCreate_v2");
    LOAD(ctx_set, "cuCtxSetCurrent");
    LOAD(ctx_destroy, "cuCtxDestroy_v2");
    LOAD(mem_alloc, "cuMemAlloc_v2");
    LOAD(mem_free, "cuMemFree_v2");
    LOAD(mem_get_info, "cuMemGetInfo_v2");
    LOAD(host_alloc, "cuMemAllocHost_v2");
    LOAD(host_free, "cuMemFreeHost");
    LOAD(stream_create, "cuStreamCreate");
    LOAD(stream_sync, "cuStreamSynchronize");
    LOAD(stream_destroy, "cuStreamDestroy_v2");
    LOAD(htod, "cuMemcpyHtoDAsync_v2");
    LOAD(dtoh, "cuMemcpyDtoHAsync_v2");
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int64_t now = now_ms();
        if (now < 0 || now >= deadline) return -1;
        int64_t remain = deadline - now;
        int r = poll(&p, 1, remain > INT_MAX ? INT_MAX : (int)remain);
        if (r < 0 && errno == EINTR) continue;
        return r == 1 && (p.revents & events) ? 0 : -1;
    }
}

/* Returns 1 only for EOF before any bytes, and -1 for a truncated/error read. */
static int read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + 10000;
    while (done < n) {
        if (wait_fd(fd, POLLIN, deadline)) return -1;
        ssize_t r = recv(fd, (char *)buf + done, n - done, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (!r) return done ? -1 : 1;
        if (r < 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + 10000;
    while (done < n) {
        if (wait_fd(fd, POLLOUT, deadline)) return -1;
        ssize_t r = send(fd, (const char *)buf + done, n - done, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int valid_context(struct server *s, const struct cb_message *m)
{
    return s->context && m->ctx == 1;
}

static CUresult set_context(struct server *s, const struct cb_message *m)
{
    if (!valid_context(s, m)) return CUDA_ERROR_INVALID_CONTEXT;
    return s->d.ctx_set(s->context);
}

static CUresult get_stream(struct server *s, uint64_t id, CUstream *out)
{
    if (id == 0) { *out = NULL; return CUDA_SUCCESS; }
    if (id > 16 || !s->streams[id - 1]) return CUDA_ERROR_INVALID_HANDLE;
    *out = s->streams[id - 1];
    return CUDA_SUCCESS;
}

static int bad_payload(const struct cb_message *m)
{
    if (m->length > CB_MAX_DATA) return 1;
    if (m->op == CB_HTOD) return m->length != m->n || m->n > CB_MAX_DATA;
    if (m->op == CB_DTOH && m->n > CB_MAX_DATA) return 1;
    return m->length != 0;
}

static int handle_request(struct server *s, int fd, struct cb_message *m)
{
    unsigned char data[CB_MAX_DATA];
    CUresult r = CUDA_SUCCESS;
    CUstream stream = NULL;
    size_t n;
    int send_data = 0;
    if (m->magic != CB_MAGIC || m->version != CB_VERSION || m->op < CB_INIT ||
        m->op > CB_DTOH || s->seq == UINT64_MAX || m->seq != s->seq + 1 || m->status || m->reserved ||
        bad_payload(m)) return -1;
    if (m->op == CB_HTOD && read_full(fd, data, m->length)) return -1;
    s->seq = m->seq;
    switch (m->op) {
    case CB_INIT:
        if (m->ctx || m->b || m->n) return -1;
        r = m->a > UINT_MAX || m->a ? CUDA_ERROR_INVALID_VALUE : s->d.init(0);
        if (!r) s->initialized = 1;
        break;
    case CB_DEVICE:
        if (m->ctx || m->b || m->n) return -1;
        if (!s->initialized) { r = CUDA_ERROR_NOT_INITIALIZED; break; }
        if (m->a) { r = CUDA_ERROR_INVALID_DEVICE; break; }
        r = s->d.device_get(&s->device, 0);
        if (!r) { s->device_valid = 1; m->a = 0; }
        break;
    case CB_CTX_CREATE:
        if (m->n) return -1;
        if (m->ctx) { r = CUDA_ERROR_INVALID_CONTEXT; break; }
        if (m->a != 0 && m->a != 4) { r = CUDA_ERROR_INVALID_VALUE; break; }
        if (m->b > INT32_MAX) { r = CUDA_ERROR_INVALID_VALUE; break; }
        if (s->context_created) { r = CUDA_ERROR_INVALID_CONTEXT; break; }
        if (!s->initialized) { r = CUDA_ERROR_NOT_INITIALIZED; break; }
        if (!s->device_valid) { r = CUDA_ERROR_INVALID_VALUE; break; }
        if (m->b != (uint64_t)(unsigned int)s->device) { r = CUDA_ERROR_INVALID_VALUE; break; }
        r = s->d.ctx_create(&s->context, (unsigned int)m->a, s->device);
        if (!r) {
            s->context_created = 1;
            r = s->d.ctx_set(s->context);
            if (!r) r = s->d.host_alloc(&s->stage, CB_MAX_DATA);
            if (r) { (void)s->d.ctx_destroy(s->context); s->context = NULL; s->stage = NULL; }
            else m->a = 1;
        }
        break;
    case CB_CTX_SET:
        if (m->a || m->b || m->n) return -1;
        r = set_context(s, m);
        break;
    case CB_CTX_DESTROY:
        if (m->a || m->b || m->n) return -1;
        r = set_context(s, m);
        if (!r) {
            for (size_t i = 0; i < 16; ++i) if (s->streams[i]) {
                CUresult x = s->d.stream_destroy(s->streams[i]);
                if (!r) r = x;
                s->streams[i] = NULL;
            }
            if (s->allocation) { CUresult x = s->d.mem_free(s->allocation); if (!r) r = x; s->allocation = 0; }
            if (s->stage) { CUresult x = s->d.host_free(s->stage); if (!r) r = x; s->stage = NULL; }
            { CUresult x = s->d.ctx_destroy(s->context); if (!r) r = x; s->context = NULL; }
        }
        break;
    case CB_ALLOC:
        if (m->a || m->b || !m->n || m->n > CB_GPU_LIMIT || s->allocation || s->allocation_max) {
            r = CUDA_ERROR_INVALID_VALUE; break;
        }
        r = set_context(s, m);
        if (!r) {
            n = (size_t)m->n;
            size_t free_bytes, total_bytes;
            r = s->d.mem_get_info(&free_bytes, &total_bytes);
            (void)total_bytes;
            const size_t headroom = (size_t)2 * 1024 * 1024 * 1024;
            if (r) break;
            if (free_bytes < headroom || n > free_bytes - headroom) {
                r = CUDA_ERROR_OUT_OF_MEMORY;
                break;
            }
            r = s->d.mem_alloc(&s->allocation, n);
            if (!r) { s->allocation_size = n; s->allocation_max = n; m->a = CB_DEVICE_BASE; }
        }
        break;
    case CB_FREE:
        if (m->b || m->n || m->a != CB_DEVICE_BASE || !s->allocation) {
            r = CUDA_ERROR_INVALID_VALUE; break;
        }
        r = set_context(s, m);
        if (!r) { r = s->d.mem_free(s->allocation); if (!r) { s->allocation = 0; s->allocation_size = 0; } }
        break;
    case CB_STREAM_CREATE:
        if (m->b || m->n || m->a > 1) { r = CUDA_ERROR_INVALID_VALUE; break; }
        r = set_context(s, m);
        if (!r) {
            size_t i;
            for (i = 0; i < 16 && s->streams[i]; ++i) {}
            if (i == 16) r = CUDA_ERROR_OUT_OF_MEMORY;
            else { r = s->d.stream_create(&s->streams[i], (unsigned int)m->a); if (!r) m->a = i + 1; }
        }
        break;
    case CB_STREAM_SYNC:
        if (m->b || m->n) { r = CUDA_ERROR_INVALID_VALUE; break; }
        r = set_context(s, m);
        if (!r) r = get_stream(s, m->a, &stream);
        if (!r) r = s->d.stream_sync(stream);
        break;
    case CB_STREAM_DESTROY:
        if (m->b || m->n) { r = CUDA_ERROR_INVALID_VALUE; break; }
        r = set_context(s, m);
        if (!r && (m->a == 0 || m->a > 16 || !s->streams[m->a - 1])) r = CUDA_ERROR_INVALID_HANDLE;
        if (!r) { r = s->d.stream_destroy(s->streams[m->a - 1]); if (!r) s->streams[m->a - 1] = NULL; }
        break;
    case CB_INFO:
        if (m->a || m->b || m->n) { r = CUDA_ERROR_INVALID_VALUE; break; }
        r = set_context(s, m);
        if (!r) {
            size_t free_bytes, total_bytes;
            r = s->d.mem_get_info(&free_bytes, &total_bytes);
            if (!r) { m->a = free_bytes; m->b = total_bytes; }
        }
        break;
    case CB_HTOD:
    case CB_DTOH: {
        uint64_t offset;
        if (!m->n || m->n > CB_MAX_DATA || !s->allocation || m->a < CB_DEVICE_BASE) {
            r = CUDA_ERROR_INVALID_VALUE; break;
        }
        offset = m->a - CB_DEVICE_BASE;
        if (offset > s->allocation_size || m->n > s->allocation_size - offset) {
            r = CUDA_ERROR_INVALID_VALUE; break;
        }
        if (offset > ULLONG_MAX - s->allocation) { r = CUDA_ERROR_INVALID_VALUE; break; }
        if (m->b > 16) { r = CUDA_ERROR_INVALID_HANDLE; break; }
        r = set_context(s, m);
        if (!r) r = get_stream(s, m->b, &stream);
        if (!r) {
            n = (size_t)m->n;
            if (m->op == CB_HTOD) {
                memcpy(s->stage, data, n);
                r = s->d.htod(s->allocation + offset, s->stage, n, stream);
            } else {
                r = s->d.dtoh(s->stage, s->allocation + offset, n, stream);
            }
            if (!r) r = s->d.stream_sync(stream);
            if (!r) {
                ++s->copies;
                if (m->op == CB_DTOH) { memcpy(data, s->stage, n); send_data = 1; }
            }
        }
        break;
    }
    default: return -1;
    }
    m->status = (uint32_t)r;
    m->length = send_data ? (uint32_t)m->n : 0;
    if (write_full(fd, m, sizeof(*m))) return -1;
    return !send_data || !write_full(fd, data, m->length) ? 0 : -1;
}

static void cleanup(struct server *s)
{
    if (s->context) {
        (void)s->d.ctx_set(s->context);
        for (size_t i = 0; i < 16; ++i) if (s->streams[i]) (void)s->d.stream_destroy(s->streams[i]);
        if (s->allocation) (void)s->d.mem_free(s->allocation);
        if (s->stage) (void)s->d.host_free(s->stage);
        (void)s->d.ctx_destroy(s->context);
    }
    if (s->d.handle) dlclose(s->d.handle);
    fprintf(stderr, "CUDA_BRIDGE_SERVER summary gpu_alloc_max=%zu copy_count=%u\n",
            s->allocation_max, s->copies);
}

int main(int argc, char **argv)
{
    const char *socket_path = NULL, *driver_path = NULL;
    int real_cuda = 0, listener = -1, client = -1, rc = 1, bound = 0;
    struct stat owned = {0};
    struct server s = {0};
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--real-cuda")) real_cuda = 1;
        else if ((!strcmp(argv[i], "--socket") || !strcmp(argv[i], "--driver")) && i + 1 < argc) {
            const char **dst = !strcmp(argv[i], "--socket") ? &socket_path : &driver_path;
            if (*dst) { fprintf(stderr, "duplicate option\n"); goto out; }
            *dst = argv[++i];
        } else { fprintf(stderr, "usage: %s --socket PATH --driver ABSOLUTE_PATH [--real-cuda]\n", argv[0]); goto out; }
    }
    if (geteuid() == 0) { fprintf(stderr, "refusing to run as euid 0\n"); goto out; }
    if (!socket_path || !driver_path || driver_path[0] != '/' ||
        (!real_cuda && strcmp(strrchr(driver_path, '/') ? strrchr(driver_path, '/') + 1 : driver_path,
                               "libcuda-fixture.so.1"))) {
        fprintf(stderr, "driver must be absolute; fixture basename must be libcuda-fixture.so.1\n"); goto out;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "socket path too long\n"); goto out;
    }
    strcpy(parent, socket_path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    struct stat parent_stat;
    if (lstat(parent, &parent_stat) || !S_ISDIR(parent_stat.st_mode) ||
        parent_stat.st_uid != geteuid() || (parent_stat.st_mode & 077) != 0) {
        fprintf(stderr, "socket parent must be a private directory owned by this uid\n"); goto out;
    }
    if (load_driver(&s, driver_path)) goto out;
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) { perror("socket"); goto out; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, socket_path);
    if (bind(listener, (struct sockaddr *)&addr,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1))) {
        perror("bind"); goto out;
    }
    bound = 1;
    if (lstat(socket_path, &owned) || !S_ISSOCK(owned.st_mode)) {
        fprintf(stderr, "cannot verify bound socket ownership\n"); goto out;
    }
    if (chmod(socket_path, 0600) || listen(listener, 1)) {
        perror("listen/socket ownership"); goto out;
    }
    fprintf(stderr, "CUDA_BRIDGE_SERVER backend=%s payload_limit=%u\n",
            real_cuda ? "real" : "fixture", CB_GPU_LIMIT);
    client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) { perror("accept"); goto out; }
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) || peer.uid != geteuid()) {
        fprintf(stderr, "rejecting peer with a different uid\n"); goto out;
    }
    for (;;) {
        struct cb_message m;
        /* Disk-only guest reclaim can leave CUDA idle after payload retirement. */
        int64_t idle_start = now_ms();
        if (idle_start < 0 || wait_fd(client, POLLIN, idle_start + 120000)) {
            fprintf(stderr, "connection idle timeout/error\n"); break;
        }
        int rr = read_full(client, &m, sizeof(m));
        if (rr == 1) { rc = 0; break; }
        if (rr < 0) { fprintf(stderr, "truncated frame or read timeout/error\n"); break; }
        if (handle_request(&s, client, &m)) { fprintf(stderr, "protocol/connection failure\n"); break; }
    }
out:
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    cleanup(&s);
    if (bound) {
        struct stat now;
        if (!lstat(socket_path, &now) && now.st_dev == owned.st_dev && now.st_ino == owned.st_ino)
            (void)unlink(socket_path);
    }
    return rc;
}
