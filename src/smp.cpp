/*
 * smp.cpp - BCM2837 secondary-core release for the µOS++ SMP port.
 *
 * Works for both environments:
 *   - QEMU raspi2b: all four cores enter _start and cores 1-3 park in
 *     secondary_park polling __smp_spin[core]. Writing the entry + SEV wakes
 *     them.
 *   - Real hardware: cores 1-3 are held by the GPU armstub spin loop, released
 *     by writing an entry address to their mailbox-3 set register. We point
 *     them at _start so they run the same HYP->SVC / SMP-enable path, then
 *     reach secondary_park (where __smp_spin is already set) and dispatch.
 */
#include "smp.hpp"
#include <bcm2837.hpp>

extern "C" void _start(void);
extern "C" volatile std::uint32_t _port_core_alive[OS_NCPU];

extern "C" {
volatile std::uint32_t g_core_stage[OS_NCPU] = {0, 0, 0, 0};
}

namespace {

// Clean (write back) one cache line by VA to the Point of Coherency so a core
// polling with MMU/caches OFF (secondary_park before its own mmu_enable) can
// see the store. DCCMVAC.
inline void clean_dcache_mva(std::uint32_t addr) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr) : "memory");
}

std::uint32_t entry_for(unsigned core)
{
    switch (core) {
        case 1: return reinterpret_cast<std::uint32_t>(&core1_entry);
        case 2: return reinterpret_cast<std::uint32_t>(&core2_entry);
        case 3: return reinterpret_cast<std::uint32_t>(&core3_entry);
        default: return 0;
    }
}

// Release ONE secondary core and wait until it is alive (first timer tick) so
// its initial context switch completes before the next core is released. This
// serialization avoids the simultaneous first-switch idle-thread claim race.
void release_one(unsigned core)
{
    using namespace bcm;
    __smp_spin[core] = entry_for(core);
    dsb();
    // Cores 1-3 poll __smp_spin from secondary_park BEFORE enabling their own
    // MMU/caches, so core 0's write would otherwise sit dirty in its write-back
    // L1 and never reach DRAM. Clean the line to PoC before the SEV below.
    clean_dcache_mva(reinterpret_cast<std::uint32_t>(&__smp_spin[core]));
    dsb();
    // Real hardware: release the armstub-held core via mailbox-3 (jumps to
    // _start -> secondary_park -> coreN_entry). Harmless on QEMU.
    mmio_write(local::MBOX_SET0 + 0x10u * core + 4u * local::SPIN_MBOX,
               reinterpret_cast<std::uint32_t>(&_start));
    dsb();
    sev();  // wake a QEMU core already parked in secondary_park

    // Wait (bounded) for this core to start ticking.
    for (unsigned spin = 0; spin < 20000000u; ++spin) {
        __asm__ volatile("dmb" ::: "memory");
        if (_port_core_alive[core])
            break;
        for (volatile unsigned d = 0; d < 50u; ++d) { }
        __asm__ volatile("sev" ::: "memory");
    }
}

} // namespace

namespace smp {

void start_secondary_cores()
{
    for (unsigned c = 1; c < OS_NCPU; ++c)
        release_one(c);
}

} // namespace smp
