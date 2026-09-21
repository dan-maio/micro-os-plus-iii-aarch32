//
// mmu.cpp - ARMv7-A short-descriptor MMU for the Raspberry Pi Zero 2W
//           (BCM2837, Cortex-A53 in AArch32). Flat VA==PA 1 MiB sections:
//
//   Pi Zero 2W (default, 512 MB):
//     0x00000000 .. 0x1FFFFFFF  512 MiB DRAM  : Normal WBWA, Shareable
//     0x20000000 .. 0x3EFFFFFF  (unpopulated) : Device, XN
//     0x3F000000 .. 0x3FFFFFFF  peripherals   : Device, XN
//     0x40000000 .. 0x40FFFFFF  ARM local     : Device, XN
//
//   Pi 3 B (BOARD_RPI3B, 1 GB): the whole 0x00000000 .. 0x3EFFFFFF range is
//   DRAM (stops below the 0x3F000000 peripheral block):
//     0x00000000 .. 0x3EFFFFFF  ~1008 MiB DRAM  : Normal WBWA, Shareable
//     0x3F000000 .. 0x3FFFFFFF  peripherals     : Device, XN
//     0x40000000 .. 0x40FFFFFF  ARM local       : Device, XN
//
// The Shareable + cacheable (WBWA) DRAM mapping is REQUIRED for correct SMP:
// the Cortex-A53 global exclusive monitor and the LDREX/STREX kernel lock only
// work across cores when the lock memory is Normal, Inner-Shareable, cacheable,
// and every core has joined the coherency domain (CPUECTLR.SMPEN — set in
// startup.S). Unaligned access is enabled (SCTLR.A = 0).
//

#include <cstdint>
#include <mmu.hpp>

// L1 translation table, 16 KiB / 16 KiB-aligned, reserved in the linker script.
extern "C" std::uint32_t __ttb_base[4096];

namespace {

using std::uint32_t;

// --- board memory map ------------------------------------------------------
// BOARD_RPI3B (make BOARD=rpi3b): Raspberry Pi 3 B, 1 GB DRAM (top of usable
// RAM sits just below the 0x3F000000 peripheral window). Default: Pi Zero 2W,
// 512 MB DRAM.
#ifdef BOARD_RPI3B
constexpr uint32_t kDramTop     = 0x3F000000u; // ~1008 MiB usable (1 GB board)
#else
constexpr uint32_t kDramTop     = 0x20000000u; // 512 MiB
#endif
constexpr uint32_t kMmioBase    = 0x3F000000u; // legacy peripheral block
constexpr uint32_t kMmioTop     = 0x41000000u; // through ARM local peripherals

// --- short-descriptor SECTION attribute bits (TEX remap OFF, SCTLR.TRE=0) ---
namespace bits {
constexpr uint32_t kSection = 2u << 0;   // bits[1:0]=0b10 : 1 MiB section
constexpr uint32_t kApRwAll = 3u << 10;  // AP[1:0]=11 : full read/write
constexpr uint32_t kDomain0 = 0u << 5;   // domain 0
constexpr uint32_t kXn      = 1u << 4;   // eXecute-Never
constexpr uint32_t kShared  = 1u << 16;  // Shareable

// Memory type = (TEX[14:12], C[3], B[2]).
constexpr uint32_t kDevice     = (0u << 12) | (0u << 3) | (1u << 2); // Device
constexpr uint32_t kNormalWbwa = (1u << 12) | (1u << 3) | (1u << 2); // Normal WB write-alloc
} // namespace bits

constexpr uint32_t kDramAttr =
    bits::kSection | bits::kApRwAll | bits::kDomain0 | bits::kNormalWbwa | bits::kShared;
constexpr uint32_t kDeviceAttr =
    bits::kSection | bits::kApRwAll | bits::kDomain0 | bits::kDevice | bits::kXn;

// --- CP15 / barrier helpers ------------------------------------------------
inline void dsb() noexcept { __asm__ volatile("dsb" ::: "memory"); }
inline void isb() noexcept { __asm__ volatile("isb" ::: "memory"); }

inline void write_tlbiall()        { __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0u) : "memory"); }
inline void write_bpiall()         { __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0u) : "memory"); }
inline void write_iciallu()        { __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0u) : "memory"); }
inline void write_dacr(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(v) : "memory"); }
inline void write_ttbcr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(v) : "memory"); }
inline void write_ttbr0(uint32_t v){
    // TTBR0 attributes: Inner WBWA (IRGN=01 -> bit6=0,bit0=1), Outer WBWA
    // (RGN=01 -> bits[4:3]=01), Shareable (S=bit1), so the table walks are
    // cacheable+shareable to match the DRAM mapping.
    uint32_t ttbr = v | (1u << 6) | (1u << 3) | (1u << 1) | (1u << 0);
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(ttbr) : "memory");
}

inline uint32_t read_sctlr()       { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v)); return v; }
inline void write_sctlr(uint32_t v){ __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(v) : "memory"); }

inline void clean_dcache_mva(uint32_t addr) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr) : "memory"); // DCCMVAC
}

// Build the flat 1:1 section table. Primary core only.
void build_table() {
    for (uint32_t i = 0; i < 4096u; ++i) {
        const uint32_t base = i << 20; // section base = i * 1 MiB
        if (base < kDramTop)
            __ttb_base[i] = base | kDramAttr;    // DRAM : Normal WBWA Shareable
        else if (base >= kMmioBase && base < kMmioTop)
            __ttb_base[i] = base | kDeviceAttr;  // peripherals / local : Device, XN
        else
            __ttb_base[i] = 0u;                  // invalid -> translation fault
    }
    dsb(); // publish the table before the MMU walks it
}

// Clean the data cache over [start, start+size) by MVA to Point of Coherency.
void clean_cache_range(uint32_t start, uint32_t size) {
    constexpr uint32_t line = 64u; // Cortex-A53 cache line
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

    write_dacr(0x1u);          // domain 0 = client
    write_ttbcr(0u);           // use TTBR0 for the whole address space
    write_ttbr0(static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(__ttb_base)));
    isb();

    uint32_t sctlr = read_sctlr();
    sctlr |=  (1u << 0);   // M : MMU enable
    sctlr &= ~(1u << 1);   // A : alignment fault DISABLED
    sctlr &= ~(1u << 13);  // V : low vectors (use VBAR)
    sctlr |=  (1u << 2);   // C : data/unified cache
    sctlr |=  (1u << 12);  // I : instruction cache
    sctlr |=  (1u << 11);  // Z : branch prediction
    write_sctlr(sctlr);
    isb();
}

// Primary core: build the table, then enable the MMU on this core.
extern "C" void mmu_init() {
    build_table();
    clean_cache_range(static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(__ttb_base)),
                      0x4000u); // flush the 16 KB L1 table to PoC
    mmu_enable();
}
