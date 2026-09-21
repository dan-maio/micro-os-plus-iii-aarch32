/**
 * @file dma_pool.hpp
 * @brief Allocator over the Normal-NON-CACHEABLE .dma_nc linker region.
 *
 * Every USB DMA buffer must come from here: the region is mapped NC by the
 * variant's MMU table, so DWC2 DMA needs no cache maintenance while the
 * rest of DRAM stays write-back cacheable (plan FULL_STATEOS.md §2.2,
 * option B). The U-Boot-style gadget stack allocates its DMA buffers with
 * memalign(); the env shim routes memalign() here, so the USB sources
 * compile unchanged.
 *
 * First-fit free-list allocator, 64-byte (cache-line) header/granularity.
 * Thread-safety: guarded by a StateOS mutex — callable from any task
 * (NOT from ISR context; Phase-2 USB runs polled in a task).
 */
#pragma once

#include <cstddef>
#include <cstdint>

/// First-fit allocator over the Non-Cacheable .dma_nc region (see file header).
namespace dma_pool {

/**
 * @brief Allocate DMA-safe memory from the .dma_nc region.
 * @param size  Number of bytes to allocate.
 * @param align Alignment in bytes (power of two, <= 4096; default 64 = cache line).
 * @return Pointer inside .dma_nc, or nullptr when the pool is exhausted.
 */
void* alloc(std::size_t size, std::size_t align = 64) noexcept;

/**
 * @brief Return memory to the pool.
 * @param p Pointer previously returned by alloc(); nullptr is a no-op.
 */
void free_(void* p) noexcept;

/**
 * @brief Test whether a pointer lies inside the .dma_nc region.
 * @param p Pointer to test.
 * @return true if @p p is within the pool (i.e. was or could be pool memory).
 */
bool owns(const void* p) noexcept;

/**
 * @brief Query free space (diagnostics).
 * @return Bytes currently available in the pool.
 */
std::size_t available() noexcept;

} // namespace dma_pool
