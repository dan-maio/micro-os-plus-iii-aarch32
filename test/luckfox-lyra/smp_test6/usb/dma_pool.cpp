/** @file dma_pool.cpp — see dma_pool.hpp. */

#include "dma_pool.hpp"
#include "osal.hpp"

extern "C" char __dma_nc_start[];     ///< Linker symbol: start of the .dma_nc region.
extern "C" char __dma_nc_end[];       ///< Linker symbol: end of the .dma_nc region.
extern "C" char __dma_pool_start[];   ///< Linker symbol: pool start (after the static .dma_nc buffers).

namespace dma_pool {
namespace {

constexpr std::size_t kGran = 64;   // header size and allocation granularity

struct Block {                       // one per free/used chunk, 64-B aligned
    std::size_t size;                // payload bytes (multiple of kGran)
    Block*      next;                // next FREE block (free list only)
    std::uint32_t magic;             // used-block tag for free() validation
    void*       raw;                 // block header (for aligned allocations)
};
static_assert(sizeof(Block) <= kGran);

constexpr std::uint32_t kUsedMagic = 0xD3A07001u;

Block* free_list = nullptr;
bool   inited    = false;

osal::Mutex& mtx() { static osal::Mutex m; return m; }

std::size_t round_up(std::size_t n) { return (n + kGran - 1) & ~(kGran - 1); }

void init_once() {
    if (inited) return;
    auto* b = reinterpret_cast<Block*>(__dma_pool_start);
    b->size  = static_cast<std::size_t>(__dma_nc_end - __dma_pool_start) - kGran;
    b->next  = nullptr;
    b->magic = 0;
    free_list = b;
    inited = true;
}

} // namespace

void* alloc(std::size_t size, std::size_t align) noexcept {
    if (size == 0 || align == 0 || (align & (align - 1)) || align > 4096)
        return nullptr;
    if (align < kGran) align = kGran;
    // worst case we must skew the payload inside the block to hit `align`
    const std::size_t need = round_up(size) + (align > kGran ? align : 0);

    osal::LockGuard g{mtx()};
    init_once();

    for (Block* b = free_list, *prev = nullptr; b; prev = b, b = b->next) {
        if (b->size < need) continue;

        // split the tail off if it still fits a header + one granule
        if (b->size >= need + 2 * kGran) {
            auto* tail = reinterpret_cast<Block*>(
                reinterpret_cast<char*>(b) + kGran + need);
            tail->size  = b->size - need - kGran;
            tail->next  = b->next;
            tail->magic = 0;
            b->size = need;
            b->next = tail;
        }
        // unlink b
        (prev ? prev->next : free_list) = b->next;

        b->magic = kUsedMagic;
        b->next  = nullptr;

        auto payload = reinterpret_cast<std::uintptr_t>(b) + kGran;
        payload = (payload + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        // stash the header right behind the (possibly skewed) payload
        reinterpret_cast<void**>(payload)[-1] = b;
        b->raw = b;
        return reinterpret_cast<void*>(payload);
    }
    return nullptr;
}

void free_(void* p) noexcept {
    if (!p) return;
    osal::LockGuard g{mtx()};
    auto* b = static_cast<Block*>(reinterpret_cast<void**>(p)[-1]);
    if (!owns(b) || b->magic != kUsedMagic)
        return;                       // not ours / double free — ignore
    b->magic = 0;
    b->next  = free_list;             // LIFO free list (no coalescing:
    free_list = b;                    // USB allocs are few and same-sized)
}

bool owns(const void* p) noexcept {
    return p >= static_cast<const void*>(__dma_nc_start) &&
           p <  static_cast<const void*>(__dma_nc_end);
}

std::size_t available() noexcept {
    osal::LockGuard g{mtx()};
    init_once();
    std::size_t n = 0;
    for (Block* b = free_list; b; b = b->next) n += b->size;
    return n;
}

} // namespace dma_pool
