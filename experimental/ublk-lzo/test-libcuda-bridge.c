#define _GNU_SOURCE
#include "test-cuda-bridge-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { E_INVALID = 1, E_OOM = 2, E_NOT_INIT = 3, E_BAD_DEVICE = 101,
       E_BAD_CONTEXT = 201, E_BAD_HANDLE = 400, E_UNKNOWN = 999,
       HOST_SLOTS = 256 };
typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;

static pthread_mutex_t rpc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static int channel = -1, socket_mode, failed, initialized;
static uint64_t sequence;
static size_t host_used;
static struct { void *ptr; size_t size; } host_blocks[HOST_SLOTS];
static _Thread_local uintptr_t current_context;

static int wait_fd(int fd, short events, int64_t deadline)
{
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        int64_t left = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { .fd = fd, .events = events };
        int r = poll(&p, 1, left > 10000 ? 10000 : (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (!r) { errno = ETIMEDOUT; return -1; }
        if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { errno = EIO; return -1; }
        if (p.revents & events) return 0;
    }
}

static int64_t deadline_after_10s(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000 + 10000;
}

static int exact_io(int writing, void *buf, size_t len, int64_t deadline)
{
    size_t done = 0;
    while (done < len) {
        if (wait_fd(channel, writing ? POLLOUT : POLLIN, deadline)) return -1;
        ssize_t n;
        if (writing && socket_mode) n = send(channel, (char *)buf + done, len - done, MSG_NOSIGNAL);
        else if (writing) {
            sigset_t set, old, pending;
            sigemptyset(&set); sigaddset(&set, SIGPIPE);
            if (pthread_sigmask(SIG_BLOCK, &set, &old)) return -1;
            sigpending(&pending);
            int had_sigpipe = sigismember(&pending, SIGPIPE);
            n = write(channel, (char *)buf + done, len - done);
            if (n < 0 && errno == EPIPE && !had_sigpipe) {
                struct timespec zero = {0};
                (void)sigtimedwait(&set, NULL, &zero);
            }
            (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
        } else n = read(channel, (char *)buf + done, len - done);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) { if (!n) errno = EPIPE; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int open_channel(void)
{
    const char *dev = getenv("CUDA_BRIDGE_DEVICE"), *sock = getenv("CUDA_BRIDGE_SOCKET");
    if (dev && sock) { errno = EINVAL; return -1; }
    if (sock) {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        if (!*sock || strlen(sock) >= sizeof(addr.sun_path)) { errno = EINVAL; return -1; }
        strcpy(addr.sun_path, sock);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) { int e=errno; close(fd); errno=e; return -1; }
        if (fcntl(fd, F_SETFL, O_NONBLOCK)) { int e=errno; close(fd); errno=e; return -1; }
        socket_mode = 1; return fd;
    }
    if (!dev) dev = "/dev/virtio-ports/vram.cuda";
    int fd = open(dev, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) { int e=errno ? errno : ENODEV; close(fd); errno=e; return -1; }
    socket_mode = 0; return fd;
}

static int rpc(uint32_t op, uint64_t ctx, uint64_t a, uint64_t b, uint64_t n,
               const void *out, void *in, uint32_t *status_out, struct cb_message *reply_out)
{
    int ret = -1;
    pthread_mutex_lock(&rpc_lock);
    if (failed) goto done;
    if (channel < 0 && (channel = open_channel()) < 0) { failed = 1; goto done; }
    struct cb_message q = { .magic=CB_MAGIC, .version=CB_VERSION, .op=op,
        .length=(op == CB_HTOD ? (uint32_t)n : 0), .seq=++sequence, .ctx=ctx,
        .a=a, .b=b, .n=n };
    int64_t deadline = deadline_after_10s();
    if (deadline < 0 || exact_io(1, &q, sizeof(q), deadline) ||
        (op == CB_HTOD && exact_io(1, (void *)out, (size_t)n, deadline))) goto broken;
    struct cb_message r;
    if (exact_io(0, &r, sizeof(r), deadline) || r.magic != CB_MAGIC ||
        r.version != CB_VERSION || r.op != op || r.seq != q.seq || r.ctx != ctx || r.reserved ||
        (r.length != 0 && (op != CB_DTOH || r.status || r.length != n))) goto broken;
    if (r.status == 0 && op == CB_DTOH && r.length != n) goto broken;
    if (r.status != 0 && r.length != 0) goto broken;
    if (r.status == 0 && op == CB_DTOH && exact_io(0, in, (size_t)n, deadline)) goto broken;
    if (status_out) *status_out = r.status;
    if (reply_out) *reply_out = r;
    ret = 0; goto done;
broken:
    failed = 1;
done:
    pthread_mutex_unlock(&rpc_lock);
    return ret;
}

static int call(uint32_t op, uint64_t ctx, uint64_t a, uint64_t b, uint64_t n,
                const void *out, void *in, struct cb_message *reply)
{
    uint32_t status;
    if (rpc(op, ctx, a, b, n, out, in, &status, reply)) return E_UNKNOWN;
    return (int)status;
}

static void poison_channel(void)
{
    pthread_mutex_lock(&rpc_lock);
    failed=1;
    pthread_mutex_unlock(&rpc_lock);
}

CUresult cuInit(unsigned int flags)
{
    pthread_mutex_lock(&rpc_lock);
    int already = initialized;
    pthread_mutex_unlock(&rpc_lock);
    if (already) return flags ? E_INVALID : 0;
    int r = call(CB_INIT, 0, flags, 0, 0, NULL, NULL, NULL);
    if (!r) { pthread_mutex_lock(&rpc_lock); initialized=1; pthread_mutex_unlock(&rpc_lock); }
    return r;
}
CUresult cuDeviceGet(CUdevice *device, int ordinal)
{
    if (!device) return E_INVALID;
    struct cb_message r;
    int rc = call(CB_DEVICE, 0, (uint64_t)(int64_t)ordinal, 0, 0, NULL, NULL, &r);
    if (!rc) *device = (CUdevice)r.a;
    return rc;
}
CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice device)
{
    if (!ctx || current_context || device != 0) return E_INVALID;
    struct cb_message r;
    int rc = call(CB_CTX_CREATE, 0, flags, (uint64_t)(int64_t)device, 0, NULL, NULL, &r);
    if (!rc) { if (r.a != 1) { poison_channel(); return E_UNKNOWN; } current_context=1; *ctx=(CUcontext)(uintptr_t)1; }
    return rc;
}
CUresult cuCtxSetCurrent(CUcontext ctx)
{
    uintptr_t id=(uintptr_t)ctx;
    if (id > 1) return E_BAD_CONTEXT;
    int rc=call(CB_CTX_SET, id, 0, 0, 0, NULL, NULL, NULL);
    if (!rc) current_context=id;
    return rc;
}
CUresult cuCtxDestroy_v2(CUcontext ctx)
{
    if ((uintptr_t)ctx != 1) return E_BAD_CONTEXT;
    int rc=call(CB_CTX_DESTROY, 1, 0, 0, 0, NULL, NULL, NULL);
    if (!rc && current_context==1) current_context=0;
    return rc;
}
CUresult cuMemAlloc_v2(CUdeviceptr *ptr, size_t size)
{
    if (!ptr || !size) return E_INVALID;
    struct cb_message r;
    int rc=call(CB_ALLOC,current_context,0,0,size,NULL,NULL,&r);
    if (!rc) { if (r.a != CB_DEVICE_BASE) { poison_channel(); return E_UNKNOWN; } *ptr=r.a; }
    return rc;
}
CUresult cuMemFree_v2(CUdeviceptr ptr)
{ return call(CB_FREE,current_context,ptr,0,0,NULL,NULL,NULL); }
CUresult cuStreamCreate(CUstream *stream, unsigned int flags)
{
    if (!stream) return E_INVALID;
    struct cb_message r;
    int rc=call(CB_STREAM_CREATE,current_context,flags,0,0,NULL,NULL,&r);
    if (!rc) { if (!r.a || r.a > 16) { poison_channel(); return E_UNKNOWN; } *stream=(CUstream)(uintptr_t)r.a; }
    return rc;
}
CUresult cuStreamSynchronize(CUstream stream)
{ return call(CB_STREAM_SYNC,current_context,(uintptr_t)stream,0,0,NULL,NULL,NULL); }
CUresult cuStreamDestroy_v2(CUstream stream)
{ return call(CB_STREAM_DESTROY,current_context,(uintptr_t)stream,0,0,NULL,NULL,NULL); }
CUresult cuMemcpyHtoDAsync_v2(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    if (!n) return 0;
    if (!src || n>CB_MAX_DATA) return E_INVALID;
    int rc=call(CB_HTOD,current_context,dst,(uintptr_t)stream,n,src,NULL,NULL);
    const char *trace=getenv("VRAM_TEST_TRACE_IO");
    if (!rc && trace && !strcmp(trace,"1")) fprintf(stderr,"CUDA_BRIDGE_COPY op=write bytes=%zu\n",n);
    return rc;
}
CUresult cuMemcpyDtoHAsync_v2(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    if (!n) return 0;
    if (!dst || n>CB_MAX_DATA) return E_INVALID;
    unsigned char *tmp=malloc(n);
    if (!tmp) return E_OOM;
    int rc=call(CB_DTOH,current_context,src,(uintptr_t)stream,n,NULL,tmp,NULL);
    if (!rc) memcpy(dst,tmp,n);
    const char *trace=getenv("VRAM_TEST_TRACE_IO");
    if (!rc && trace && !strcmp(trace,"1")) fprintf(stderr,"CUDA_BRIDGE_COPY op=read bytes=%zu\n",n);
    free(tmp); return rc;
}
CUresult cuMemGetInfo_v2(size_t *free_bytes, size_t *total_bytes)
{
    if (!free_bytes || !total_bytes) return E_INVALID;
    struct cb_message r;
    int rc=call(CB_INFO,current_context,0,0,0,NULL,NULL,&r);
    if (!rc) {
        if (!r.b || r.a > r.b) { poison_channel(); return E_UNKNOWN; }
        const char *inject=getenv("CUDA_BRIDGE_TEST_LOW_FREE");
        if (inject && !strcmp(inject,"after-marker") && !access("/tmp/cuda-low-free",F_OK)) {
            uint64_t threshold=r.b/8;
            if (threshold > UINT64_C(2147483648)) threshold=UINT64_C(2147483648);
            if (!threshold) { poison_channel(); return E_UNKNOWN; }
            fprintf(stderr,"CUDA_BRIDGE_PRESSURE synthetic=1 actual_free=%llu reported_free=%llu total=%llu\n",
                    (unsigned long long)r.a,(unsigned long long)(threshold-1),(unsigned long long)r.b);
            r.a=threshold-1;
        }
        *free_bytes=(size_t)r.a; *total_bytes=(size_t)r.b;
    }
    return rc;
}
CUresult cuMemAllocHost_v2(void **ptr, size_t size)
{
    if (!ptr || !size || size>CB_HOST_LIMIT) return E_INVALID;
    pthread_mutex_lock(&host_lock);
    if (size>CB_HOST_LIMIT-host_used) { pthread_mutex_unlock(&host_lock); return E_OOM; }
    size_t i; for(i=0;i<HOST_SLOTS && host_blocks[i].ptr;i++) {}
    if(i==HOST_SLOTS) { pthread_mutex_unlock(&host_lock); return E_OOM; }
    void *p=NULL;
    if (posix_memalign(&p,4096,size)) { pthread_mutex_unlock(&host_lock); return E_OOM; }
    host_blocks[i].ptr=p; host_blocks[i].size=size; host_used+=size; *ptr=p;
    pthread_mutex_unlock(&host_lock);
    fprintf(stderr,"CUDA_BRIDGE_CLIENT remote=1 staging=guest_not_cuda_pinned\n");
    return 0;
}
CUresult cuMemFreeHost(void *ptr)
{
    if (!ptr) return E_INVALID;
    pthread_mutex_lock(&host_lock);
    for(size_t i=0;i<HOST_SLOTS;i++) if(host_blocks[i].ptr==ptr) {
        host_used-=host_blocks[i].size; free(ptr); host_blocks[i].ptr=NULL; host_blocks[i].size=0;
        pthread_mutex_unlock(&host_lock); return 0;
    }
    pthread_mutex_unlock(&host_lock); return E_INVALID;
}
CUresult cuGetErrorName(CUresult code, const char **out)
{
    if (!out) return E_INVALID;
    switch(code) {
    case 0:*out="CUDA_SUCCESS";break; case E_INVALID:*out="CUDA_ERROR_INVALID_VALUE";break;
    case E_OOM:*out="CUDA_ERROR_OUT_OF_MEMORY";break; case E_NOT_INIT:*out="CUDA_ERROR_NOT_INITIALIZED";break;
    case E_BAD_DEVICE:*out="CUDA_ERROR_INVALID_DEVICE";break; case E_BAD_CONTEXT:*out="CUDA_ERROR_INVALID_CONTEXT";break;
    case E_BAD_HANDLE:*out="CUDA_ERROR_INVALID_HANDLE";break; case E_UNKNOWN:*out="CUDA_ERROR_UNKNOWN";break;
    default:*out="CUDA_ERROR_UNKNOWN";return E_INVALID;
    }
    return 0;
}
CUresult cuGetErrorString(CUresult code, const char **out)
{ return cuGetErrorName(code,out); }
