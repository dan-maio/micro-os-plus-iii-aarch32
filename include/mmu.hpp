#pragma once
//
// mmu.hpp - ARMv7-A (Cortex-A7) MMU bring-up for the RK3506 (Luckfox Lyra B),
//           with UNALIGNED ACCESS enabled (SCTLR.A = 0).
//

#include <cstdint>

extern "C" {

// Primary core (0): build the L1 table AND enable the MMU on this core.
void mmu_init();

// Secondary cores (1, 2): enable the MMU on THIS core using the table already
// built by core 0.
void mmu_enable();

// The shared L1 translation table (16 KiB, reserved in linker.ld).
extern std::uint32_t __ttb_base[4096];

} // extern "C"

namespace mmu {

/// Direct view of the shared L1 table
inline std::uint32_t* const l1_table = __ttb_base;

/// CP15 helpers
namespace cp15 {

inline std::uint32_t get_sctlr() noexcept {
    std::uint32_t v;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

} // namespace cp15

} // namespace mmu
