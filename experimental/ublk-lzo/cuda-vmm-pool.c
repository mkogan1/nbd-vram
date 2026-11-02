#include "cuda-vmm-pool.h"
#include <cuda.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

struct block { CUmemGenericAllocationHandle handle; int created, mapped, accessible; };
struct cuda_vmm_pool {
    CUresult (*create)(CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long);
    CUresult (*map)(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long);
    CUresult (*set_access)(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
    CUresult (*address_free)(CUdeviceptr, size_t);
    CUresult (*unmap)(CUdeviceptr, size_t);
    CUresult (*release)(CUmemGenericAllocationHandle);
    CUresult (*synchronize)(void);
    CUmemAllocationProp prop;
    CUmemAccessDesc access;
    CUdeviceptr base;
    size_t bytes, block_size, count;
    int reserved;
    struct block blocks[];
};

#define LOAD(member, symbol) do { *(void **)(&member) = dlsym(driver, symbol); if (!member) return (int)CUDA_ERROR_NOT_SUPPORTED; } while (0)
static size_t block_length(const struct cuda_vmm_pool *p, size_t i)
{ size_t offset = i * p->block_size; return p->bytes - offset < p->block_size ? p->bytes - offset : p->block_size; }
static int cleanup_block(struct cuda_vmm_pool *p, size_t i)
{
    struct block *b = &p->blocks[i]; size_t len = block_length(p, i);
    if (b->mapped) {
        CUresult rc = p->unmap(p->base + i * p->block_size, len);
        if (rc) return (int)rc;
        b->mapped = 0; b->accessible = 0;
    }
    if (b->created) {
        CUresult rc = p->release(b->handle);
        if (rc) return (int)rc;
        b->created = 0;
    }
    return 0;
}
size_t cuda_vmm_pool_block_size(const struct cuda_vmm_pool *p) { return p ? p->block_size : 0; }
size_t cuda_vmm_pool_block_count(const struct cuda_vmm_pool *p) { return p ? p->count : 0; }
int cuda_vmm_pool_block_allocated(const struct cuda_vmm_pool *p, size_t i)
{ return p && i < p->count ? !!p->blocks[i].created : 0; }
int cuda_vmm_pool_commit_block(struct cuda_vmm_pool *p, size_t i)
{
    if (!p || i >= p->count) return (int)CUDA_ERROR_INVALID_VALUE;
    struct block *b = &p->blocks[i]; size_t len = block_length(p, i); CUresult rc;
    if (!b->created) {
        rc = p->create(&b->handle, len, &p->prop, 0);
        if (rc) return (int)rc;
        b->created = 1;
    }
    if (!b->mapped) {
        rc = p->map(p->base + i * p->block_size, len, 0, b->handle, 0);
        if (rc) return (int)rc;
        b->mapped = 1;
    }
    if (!b->accessible) {
        rc = p->set_access(p->base + i * p->block_size, len, &p->access, 1);
        if (rc) return (int)rc;
        b->accessible = 1;
    }
    return 0;
}
int cuda_vmm_pool_release_block(struct cuda_vmm_pool *p, size_t i)
{
    if (!p || i >= p->count) return (int)CUDA_ERROR_INVALID_VALUE;
    if (!p->blocks[i].mapped && !p->blocks[i].created) return 0;
    CUresult rc = p->synchronize();
    return rc ? (int)rc : cleanup_block(p, i);
}
int cuda_vmm_pool_destroy(struct cuda_vmm_pool **pp)
{
    if (!pp || !*pp) return (int)CUDA_ERROR_INVALID_VALUE;
    struct cuda_vmm_pool *p = *pp;
    CUresult rc = p->synchronize();
    if (rc) return (int)rc;
    for (size_t i = 0; i < p->count; ++i) { int e = cleanup_block(p, i); if (e) return e; }
    if (p->reserved) {
        rc = p->address_free(p->base, p->bytes);
        if (rc) return (int)rc;
        p->reserved = 0;
    }
    free(p); *pp = NULL; return 0;
}
int cuda_vmm_pool_reserve(struct cuda_vmm_pool **out, void *driver, int device,
                          size_t bytes, uint64_t *base)
{
    if (!out || *out || !driver || !base || !bytes) return (int)CUDA_ERROR_INVALID_VALUE;
    CUresult (*granularity)(size_t *, const CUmemAllocationProp *, CUmemAllocationGranularity_flags);
    CUresult (*reserve)(CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long);
    CUresult (*create)(CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long);
    CUresult (*map)(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long);
    CUresult (*set_access)(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
    CUresult (*address_free)(CUdeviceptr, size_t);
    CUresult (*unmap)(CUdeviceptr, size_t);
    CUresult (*release)(CUmemGenericAllocationHandle);
    CUresult (*synchronize)(void);
    LOAD(granularity, "cuMemGetAllocationGranularity"); LOAD(reserve, "cuMemAddressReserve");
    LOAD(create, "cuMemCreate"); LOAD(map, "cuMemMap"); LOAD(set_access, "cuMemSetAccess");
    LOAD(address_free, "cuMemAddressFree"); LOAD(unmap, "cuMemUnmap");
    LOAD(release, "cuMemRelease"); LOAD(synchronize, "cuCtxSynchronize");
    CUmemAllocationProp prop = {0}; prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE; prop.location.id = device;
    size_t gran = 0; CUresult rc = granularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM);
    if (rc) return (int)rc;
    if (!gran || (gran & (gran - 1)) || gran > 8u * 1024u * 1024u || bytes % gran) return (int)CUDA_ERROR_INVALID_VALUE;
    size_t block_size = gran > 2u * 1024u * 1024u ? gran : 2u * 1024u * 1024u;
    size_t count = bytes / block_size + !!(bytes % block_size);
    if (count > (SIZE_MAX - sizeof(struct cuda_vmm_pool)) / sizeof(struct block)) return (int)CUDA_ERROR_OUT_OF_MEMORY;
    struct cuda_vmm_pool *p = calloc(1, sizeof(*p) + count * sizeof(struct block));
    if (!p) return (int)CUDA_ERROR_OUT_OF_MEMORY;
    p->create = create; p->map = map; p->set_access = set_access;
    p->address_free = address_free; p->unmap = unmap; p->release = release; p->synchronize = synchronize;
    p->prop = prop; p->access.location = prop.location; p->access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    p->bytes = bytes; p->block_size = block_size; p->count = count;
    rc = reserve(&p->base, bytes, gran, 0, 0);
    if (rc) { free(p); return (int)rc; }
    p->reserved = 1; *base = (uint64_t)p->base; *out = p; return 0;
}
int cuda_vmm_pool_create(struct cuda_vmm_pool **out, void *driver, int device,
                         size_t bytes, uint64_t *base)
{
    if (!out || *out || !base) return (int)CUDA_ERROR_INVALID_VALUE;
    struct cuda_vmm_pool *p = NULL; uint64_t reserved_base;
    int rc = cuda_vmm_pool_reserve(&p, driver, device, bytes, &reserved_base);
    if (rc) return rc;
    for (size_t i = 0; i < p->count; ++i) {
        rc = cuda_vmm_pool_commit_block(p, i);
        if (rc) {
            int original = rc, pending = 0;
            if (!p->synchronize()) {
                for (size_t j = 0; j < p->count; ++j) if (cleanup_block(p, j)) pending = 1;
                if (!pending && p->reserved && !p->address_free(p->base, p->bytes)) p->reserved = 0;
            } else pending = 1;
            if (!p->reserved && !pending) free(p); else *out = p;
            return original;
        }
    }
    *base = reserved_base; *out = p; return 0;
}
