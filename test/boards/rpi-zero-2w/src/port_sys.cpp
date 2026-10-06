/*
 * port_sys.cpp - BCM2837 board glue for the µOS++ SMP port.
 *
 * Replaces the a7 port's GIC-based port_sys.cpp. Provides the SMP lock
 * storage, per-core interrupt bring-up (generic-timer PPI routing + mailbox
 * IPI), the CPU-id helper, and the cross-core reschedule IPI over mailbox 0.
 */
#include <cstdint>
#include <cmsis-plus/rtos/port/os-decls.h>
#include <bcm_irq.hpp>
#include <timer_arm.hpp>

namespace os { namespace rtos { namespace port { namespace scheduler {

smp_klock_t _smp_klock = {0, SMP_NO_OWNER, 0};
smp_tlock_t _smp_tlock = {0};
volatile unsigned _port_ctx_pending[OS_NCPU] = {0};

}}}}

// Secondary-core launch spin-table (indexed by core id). Core 0 writes a
// launch entry + SEV to release a parked secondary (Phase 2).
extern "C" volatile std::uint32_t __smp_spin[OS_NCPU] = {0, 0, 0, 0};

extern "C" void port_sys_init(void)
{
    // Route this core's generic-timer interrupt to itself and enable the
    // mailbox-0 reschedule IPI for this core.
    bcm_irq::route_timer_this_core();
    bcm_irq::enable_ipi_this_core();

    // Program the ARM generic timer for a 1 ms tick on this core.
    timer_arm::init();
    timer_arm::start_1ms();
}

extern "C" unsigned port_cpu_id(void)
{
    unsigned mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 0xFFu;
}

extern "C" void port_smp_ipi(unsigned cpu)
{
    if (cpu < OS_NCPU) {
        bcm_irq::send_ipi(cpu);
    }
}
