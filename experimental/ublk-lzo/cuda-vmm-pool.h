#ifndef CUDA_VMM_POOL_H
#define CUDA_VMM_POOL_H
#include <stddef.h>
#include <stdint.h>

struct cuda_vmm_pool;
/* *out must be NULL; failed cleanup preserves a non-NULL pool for retry. */
int cuda_vmm_pool_reserve(struct cuda_vmm_pool **out, void *driver, int device,
                          size_t bytes, uint64_t *base);
int cuda_vmm_pool_commit_block(struct cuda_vmm_pool *pool, size_t index);
int cuda_vmm_pool_block_allocated(const struct cuda_vmm_pool *pool, size_t index);
int cuda_vmm_pool_create(struct cuda_vmm_pool **out, void *driver, int device,
                         size_t bytes, uint64_t *base);
size_t cuda_vmm_pool_block_size(const struct cuda_vmm_pool *pool);
size_t cuda_vmm_pool_block_count(const struct cuda_vmm_pool *pool);
int cuda_vmm_pool_release_block(struct cuda_vmm_pool *pool, size_t index);
int cuda_vmm_pool_destroy(struct cuda_vmm_pool **pool);
#endif
