//
// mmu.cpp - ARMv7-A short-descriptor MMU: flat VA==PA map, DRAM Normal, MMIO
//           Device, with unaligned access enabled (SCTLR.A = 0).
//

#include <cstdint>
#include <mmu.hpp>

// L1 translation table, 16 KiB / 16 KiB-aligned, reserved in the linker script.
extern "C" std::uint32_t __ttb_base[4096];

// There is no "cacheable DRAM off" build any more, and there never really was
// one: the predecessor project carried a MMU_DRAM_CACHEABLE switch that
// defaulted to 0, and then passed -DMMU_DRAM_CACHEABLE=1 in every single one
// of its thirteen Makefiles. A default nothing ever used is not a default,
// it is a trap -- and this port fell straight into it the first time the
// board ran, because CMake did not carry the -D over.
//
// The "off" setting cannot work. It maps DRAM Normal NON-CACHEABLE and, worse,
// NON-SHAREABLE, and turns SCTLR.C and SCTLR.I off as well. The kernel's
// scheduler lock is LDREX/STREX, and an exclusive access to non-shareable,
// non-cacheable memory has no usable exclusive monitor on this interconnect:
// the first LDREX the kernel executes takes a synchronous external abort
// (DFSR 0x1008). Everything that did run before that ran at raw DRAM latency
// with both caches off, which is visible to the naked eye on the console.
//
// So DRAM is Normal write-back write-allocate and Shareable, full stop --
// exactly as the BCM2837 board maps it, which is why that board has no such
// switch either.

extern "C" char __dma_nc_start[], __dma_nc_end[];

namespace {

using std::uint32_t;

// --- board memory map ------------------------------------------------------
constexpr uint32_t kDramSizeMb = 128;
constexpr uint32_t kDramTop    = kDramSizeMb << 20; // 0x08000000
constexpr uint32_t kMmioBase   = 0xFF000000u;       // peripherals / SRAM / BootROM

// --- short-descriptor SECTION attribute bits (TEX remap OFF, SCTLR.TRE=0) ---
namespace bits {
constexpr uint32_t kSection = 2u << 0;   // bits[1:0]=0b10 : 1 MiB section
constexpr uint32_t kApRwAll = 3u << 10;  // AP[1:0]=11 : full read/write
constexpr uint32_t kDomain0 = 0u << 5;   // domain 0
constexpr uint32_t kXn      = 1u << 4;   // eXecute-Never
constexpr uint32_t kShared  = 1u << 16;  // Shareable

// Memory type = (TEX[14:12], C[3], B[2]).
constexpr uint32_t kDevice     = (0u << 12) | (0u << 3) | (1u << 2); // Device
constexpr uint32_t kNormalNc   = (1u << 12) | (0u << 3) | (0u << 2); // Normal non-cacheable
constexpr uint32_t kNormalWbwa = (1u << 12) | (1u << 3) | (1u << 2); // Normal WB write-alloc
} // namespace bits

constexpr uint32_t kDramAttr =
    bits::kSection | bits::kApRwAll | bits::kDomain0 | bits::kNormalWbwa | bits::kShared;
constexpr uint32_t kDeviceAttr =
    bits::kSection | bits::kApRwAll | bits::kDomain0 | bits::kDevice | bits::kXn;

// --- CP15 / barrier helpers (each is a single instruction once inlined) -----
inline void dsb() noexcept { __asm__ volatile("dsb" ::: "memory"); }
inline void isb() noexcept { __asm__ volatile("isb" ::: "memory"); }

inline void write_tlbiall()       { __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0u) : "memory"); }
inline void write_bpiall()        { __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0u) : "memory"); }
inline void write_iciallu()       { __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0u) : "memory"); }
inline void write_dacr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(v) : "memory"); }
inline void write_ttbcr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(v) : "memory"); }
inline void write_ttbr0(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(v) : "memory"); }

inline uint32_t read_sctlr() { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v)); return v; }
inline void write_sctlr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(v) : "memory"); }
inline uint32_t read_actlr() { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(v)); return v; }
inline void write_actlr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c1, c0, 1" :: "r"(v) : "memory"); }

inline void clean_dcache_mva(uint32_t addr) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr) : "memory"); // DCCMVAC
}

// USB DMA window (linker.ld .dma_nc): always Normal NON-CACHEABLE
constexpr uint32_t kDramNcAttr =
    bits::kSection | bits::kApRwAll | bits::kDomain0 | bits::kNormalNc | bits::kShared;

// Build the flat 1:1 section table. Primary core only.
void build_table() {
    const uint32_t nc_first = reinterpret_cast<uint32_t>(__dma_nc_start) >> 20;
    const uint32_t nc_last  = (reinterpret_cast<uint32_t>(__dma_nc_end) - 1u) >> 20;

    for (uint32_t i = 0; i < 4096u; ++i) {
        const uint32_t base = i << 20; // section base = i * 1 MiB

        if (i >= nc_first && i <= nc_last)
            __ttb_base[i] = base | kDramNcAttr;  // USB DMA window : Normal NC
        else if (base < kDramTop)
            __ttb_base[i] = base | kDramAttr;    // 128 MB DDR : Normal
        else if (base >= kMmioBase)
            __ttb_base[i] = base | kDeviceAttr;  // peripherals : Device, XN
        else
            __ttb_base[i] = 0u;                  // invalid -> translation fault
    }

    dsb(); // publish the table before the MMU walks it
}

// Clean the data cache over [start, start+size) by MVA to Point of Coherency.
void clean_cache_range(uint32_t start, uint32_t size) {
    constexpr uint32_t line = 64u; // Cortex-A7 cache line
    const uint32_t end = start + size;
    start &= ~(line - 1u);
    for (uint32_t addr = start; addr < end; addr += line)
        clean_dcache_mva(addr);
    dsb();
}

} // namespace

// Program CP15 and turn the MMU on for the CURRENT core (shared table).
extern "C" void mmu_enable() {
    write_tlbiall();
    write_bpiall();
    write_iciallu();

    write_dacr(0x1u);
    write_ttbcr(0u);
    write_ttbr0(static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(__ttb_base)));

    write_actlr(read_actlr() | (1u << 6));

    isb();

    uint32_t sctlr = read_sctlr();
    sctlr |=  (1u << 0);   // M : MMU enable
    sctlr &= ~(1u << 1);   // A : alignment fault DISABLED
    sctlr &= ~(1u << 13);  // V : low vectors (use VBAR)
    sctlr |=  (1u << 2);   // C : data/unified cache
    sctlr |=  (1u << 12);  // I : instruction cache
    // SCTLR.Z is not touched here, unlike the BCM2837 board. On a Cortex-A7
    // it is RAO/WI -- branch prediction is always on -- so setting it would
    // be a no-op, and this file is otherwise exactly what the predecessor
    // project compiled.
    write_sctlr(sctlr);
    isb();
}

// Primary core: build the table, then enable the MMU on this core.
extern "C" void mmu_init() {
    build_table();
    clean_cache_range(static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(__ttb_base)),
                      0x4000u); // flush the 16 KB L1 table
    mmu_enable();
}
