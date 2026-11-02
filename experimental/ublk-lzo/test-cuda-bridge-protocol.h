/* Test-only, same-endian x86-64 peers; no native pointers cross the channel. */
#ifndef TEST_CUDA_BRIDGE_PROTOCOL_H
#define TEST_CUDA_BRIDGE_PROTOCOL_H
#include <stdint.h>
#define CB_MAGIC 0x43425231u
#define CB_VERSION 1u
#define CB_MAX_DATA 65536u
#define CB_GPU_LIMIT (2u * 1024u * 1024u)
#define CB_HOST_LIMIT (4u * 1024u * 1024u)
#define CB_DEVICE_BASE UINT64_C(0x100000000)
enum cb_op { CB_INIT=1, CB_DEVICE, CB_CTX_CREATE, CB_CTX_SET, CB_CTX_DESTROY,
 CB_ALLOC, CB_FREE, CB_STREAM_CREATE, CB_STREAM_SYNC, CB_STREAM_DESTROY,
 CB_INFO, CB_HTOD, CB_DTOH };
struct cb_message {
 uint32_t magic, version, op, length, status, reserved;
 uint64_t seq, ctx, a, b, n;
};
_Static_assert(sizeof(struct cb_message)==64, "bridge wire layout");
/* Request status/reserved=0. Response echoes op/seq/ctx, status=CUresult.
 * INIT a=flags; DEVICE a=ordinal -> a=device(0).
 * CTX_CREATE a=flags,b=device -> a=context(1); SET/DESTROY ctx=context.
 * ALLOC n=size -> a=virtual base; FREE a=virtual base.
 * STREAM_CREATE a=flags -> a=id; SYNC/DESTROY a=id (0 default for SYNC).
 * INFO -> a=free,b=total. HTOD/DTOH a=virtual address,b=stream,n=bytes.
 * Only HTOD requests and successful DTOH responses carry length=n bytes.
 * Copies complete with a real stream synchronization before response.
 * One context and one GPU allocation per connection, stream IDs 1..16.
 */
#endif
