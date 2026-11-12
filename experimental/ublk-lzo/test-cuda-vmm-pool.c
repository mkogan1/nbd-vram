#include <cuda.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *fixture_dlsym(void *, const char *);
#define dlsym fixture_dlsym
#include "cuda-vmm-pool.c"
#undef dlsym
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int fail_unmap, fail_release, fail_address_free, fail_map, fail_create, fail_access, bad_gran, missing, fail_sync;
static unsigned maps, unmaps, releases, frees, syncs, creates, accesses;
static size_t alloc_size[8192];
static CUdeviceptr map_address[8192];
static CUresult gran(size_t *v, const CUmemAllocationProp *p, CUmemAllocationGranularity_flags f)
{ CHECK(p->type == CU_MEM_ALLOCATION_TYPE_PINNED && p->location.type == CU_MEM_LOCATION_TYPE_DEVICE && p->location.id == 7 && f == CU_MEM_ALLOC_GRANULARITY_MINIMUM); *v = bad_gran ? 3000 : 65536; return 0; }
static CUresult reserve(CUdeviceptr *p, size_t n, size_t a, CUdeviceptr addr, unsigned long long flags)
{ CHECK(n && a == 65536 && !addr && !flags); *p = 0x100000; return 0; }
static CUresult create(CUmemGenericAllocationHandle *h, size_t n, const CUmemAllocationProp *p, unsigned long long flags)
{ CHECK(p->type == CU_MEM_ALLOCATION_TYPE_PINNED && !flags && creates < sizeof(alloc_size) / sizeof(*alloc_size)); ++creates; if (fail_create == (int)creates) return 76; *h = 0xabc0 + creates; alloc_size[creates - 1] = n; return 0; }
static CUresult map(CUdeviceptr p, size_t n, size_t off, CUmemGenericAllocationHandle h, unsigned long long flags)
{ CHECK(p >= 0x100000 && n && !off && h && !flags && maps < 8192); map_address[maps] = p; ++maps; return fail_map == (int)maps ? 77 : 0; }
static CUresult access(CUdeviceptr p, size_t n, const CUmemAccessDesc *a, size_t count)
{ CHECK(p >= 0x100000 && n && count == 1 && a->location.type == CU_MEM_LOCATION_TYPE_DEVICE && a->location.id == 7 && a->flags == CU_MEM_ACCESS_FLAGS_PROT_READWRITE); ++accesses; return fail_access == (int)accesses ? 75 : 0; }
static CUresult unmap(CUdeviceptr p, size_t n)
{ CHECK(p >= 0x100000 && n); ++unmaps; return fail_unmap ? 78 : 0; }
static CUresult release(CUmemGenericAllocationHandle h)
{ CHECK(h); ++releases; return fail_release ? 79 : 0; }
static CUresult address_free(CUdeviceptr p, size_t n)
{ CHECK(p == 0x100000 && n); ++frees; return fail_address_free ? 80 : 0; }
static CUresult sync(void) { ++syncs; return fail_sync ? 81 : 0; }
static void *fixture_dlsym(void *h, const char *s)
{
    if (h != (void *)0x1) abort();
#define S(n, f) if (!strcmp(s, n)) return (void *)(f)
    if (missing && !strcmp(s, "cuMemUnmap")) return NULL;
    S("cuMemGetAllocationGranularity", gran); S("cuMemAddressReserve", reserve);
    S("cuMemCreate", create); S("cuMemMap", map); S("cuMemSetAccess", access);
    S("cuMemAddressFree", address_free); S("cuMemUnmap", unmap); S("cuMemRelease", release); S("cuCtxSynchronize", sync);
#undef S
    return NULL;
}
static void reset(void) { fail_sync = fail_unmap = fail_release = fail_address_free = fail_map = fail_create = fail_access = bad_gran = missing = 0; maps = unmaps = releases = frees = syncs = creates = accesses = 0; memset(alloc_size, 0, sizeof(alloc_size)); memset(map_address, 0, sizeof(map_address)); }
int main(void)
{
    struct cuda_vmm_pool *p = NULL; uint64_t base = 0; reset();
    CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, NULL) == CUDA_ERROR_INVALID_VALUE && !p && !creates);
    CHECK(!cuda_vmm_pool_create(&p, (void *)1, 7, 5u * 1024u * 1024u, &base));
    CHECK(p && base == 0x100000 && cuda_vmm_pool_block_size(p) == 2u * 1024u * 1024u && cuda_vmm_pool_block_count(p) == 3);
    CHECK(alloc_size[0] == 2u * 1024u * 1024u && alloc_size[1] == 2u * 1024u * 1024u && alloc_size[2] == 1u * 1024u * 1024u);
    struct cuda_vmm_pool *saved = p;
    CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, &base) == CUDA_ERROR_INVALID_VALUE && p == saved);
    fail_sync = 1;
    CHECK(cuda_vmm_pool_release_block(p, 1) == 81 && !unmaps && !releases);
    CHECK(cuda_vmm_pool_destroy(&p) == 81 && p == saved && !unmaps && !releases);
    fail_sync = 0;
    fail_unmap = 1; CHECK(cuda_vmm_pool_release_block(p, 1) == 78); CHECK(unmaps == 1 && releases == 0);
    fail_unmap = 0; fail_release = 1; CHECK(cuda_vmm_pool_release_block(p, 1) == 79); CHECK(unmaps == 2 && releases == 1);
    fail_release = 0; CHECK(!cuda_vmm_pool_release_block(p, 1)); CHECK(unmaps == 2 && releases == 2);
    CHECK(!cuda_vmm_pool_release_block(p, 1)); CHECK(unmaps == 2 && releases == 2);
    fail_sync = 1; CHECK(!cuda_vmm_pool_release_block(p, 1)); fail_sync = 0;
    fail_address_free = 1; CHECK(cuda_vmm_pool_destroy(&p) == 80 && p); CHECK(unmaps == 4 && releases == 4);
    fail_address_free = 0; CHECK(!cuda_vmm_pool_destroy(&p) && !p && frees == 2 && unmaps == 4 && releases == 4);
    reset(); missing = 1; CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, &base) != 0 && !p);
    missing = 0; bad_gran = 1; CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 3000, &base) != 0 && !p);
    reset(); fail_map = 1; CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, &base) == 77 && !p); CHECK(creates == 1 && maps == 1 && unmaps == 0 && releases == 1 && frees == 1);
    reset(); fail_map = 2; fail_unmap = 1;
    CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 5u * 1024u * 1024u, &base) == 77 && p);
    CHECK(p->blocks[0].mapped && p->blocks[0].created && !p->blocks[1].created && !frees);
    fail_unmap = 0; CHECK(!cuda_vmm_pool_destroy(&p) && !p && releases == 2 && frees == 1);
    reset(); fail_map = 1; fail_release = 1;
    CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, &base) == 77 && p && p->blocks[0].created && !unmaps);
    fail_release = 0; CHECK(!cuda_vmm_pool_destroy(&p) && !p && releases == 2 && frees == 1);
    reset(); fail_map = 1; fail_address_free = 1;
    CHECK(cuda_vmm_pool_create(&p, (void *)1, 7, 65536, &base) == 77 && p && p->reserved && !p->blocks[0].created);
    fail_address_free = 0; CHECK(!cuda_vmm_pool_destroy(&p) && !p && frees == 2);
    reset(); CHECK(!cuda_vmm_pool_reserve(&p, (void *)1, 7, (size_t)UINT64_C(17179869184), &base));
    CHECK(cuda_vmm_pool_block_count(p) == 8192 && !creates && !maps && !accesses && !cuda_vmm_pool_block_allocated(p, 8191));
    CHECK(cuda_vmm_pool_commit_block(p, 8192) == CUDA_ERROR_INVALID_VALUE);
    CHECK(!cuda_vmm_pool_commit_block(p, 8191) && creates == 1 && maps == 1 && accesses == 1);
    CHECK(!cuda_vmm_pool_commit_block(p, 0) && creates == 2 && maps == 2 && accesses == 2);
    CHECK(map_address[0] == UINT64_C(0x100000) + UINT64_C(8191) * 2u * 1024u * 1024u);
    CHECK(map_address[1] == UINT64_C(0x100000));
    CHECK(!cuda_vmm_pool_commit_block(p, 8191) && creates == 2 && maps == 2 && accesses == 2);
    CHECK(cuda_vmm_pool_block_allocated(p, 8191) && cuda_vmm_pool_block_allocated(p, 0));
    CHECK(alloc_size[0] == 2u * 1024u * 1024u && alloc_size[1] == 2u * 1024u * 1024u);
    CHECK(!cuda_vmm_pool_release_block(p, 8191));
    CHECK(!cuda_vmm_pool_block_allocated(p, 8191));
    CHECK(!cuda_vmm_pool_commit_block(p, 8191) && creates == 3 && maps == 3 && accesses == 3);
    CHECK(map_address[2] == map_address[0]);
    CHECK(!cuda_vmm_pool_destroy(&p) && !p && unmaps == 3 && releases == 3 && frees == 1);
    reset(); CHECK(!cuda_vmm_pool_reserve(&p, (void *)1, 7, 65536, &base));
    fail_create = 1; CHECK(cuda_vmm_pool_commit_block(p, 0) == 76 && !cuda_vmm_pool_block_allocated(p, 0));
    fail_create = 0; fail_map = 1; CHECK(cuda_vmm_pool_commit_block(p, 0) == 77 && cuda_vmm_pool_block_allocated(p, 0));
    fail_map = 0; fail_access = 1; CHECK(cuda_vmm_pool_commit_block(p, 0) == 75 && cuda_vmm_pool_block_allocated(p, 0));
    CHECK(!p->blocks[0].accessible && p->blocks[0].mapped);
    fail_access = 0; CHECK(!cuda_vmm_pool_commit_block(p, 0) && creates == 2 && maps == 2 && accesses == 2);
    fail_unmap = 1; CHECK(cuda_vmm_pool_release_block(p, 0) == 78 && p->blocks[0].accessible);
    fail_unmap = 0; fail_release = 1; CHECK(cuda_vmm_pool_release_block(p, 0) == 79 && !p->blocks[0].accessible && p->blocks[0].created);
    fail_release = 0; CHECK(!cuda_vmm_pool_release_block(p, 0) && !p->blocks[0].accessible);
    CHECK(!cuda_vmm_pool_destroy(&p) && !p && unmaps == 2 && releases == 2);
    puts("cuda-vmm-pool CPU fixture: PASS"); return 0;
}
