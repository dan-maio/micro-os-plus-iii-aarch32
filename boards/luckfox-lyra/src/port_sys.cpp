#include <cstdint>
#include <cmsis-plus/rtos/port/os-decls.h>
#include <gic.hpp>
#include <timer_arm.hpp>

namespace os { namespace rtos { namespace port { namespace scheduler {

smp_klock_t _smp_klock = {0, SMP_NO_OWNER, 0};
smp_tlock_t _smp_tlock = {0};
volatile unsigned _port_ctx_pending[OS_NCPU] = {0};

}}}}

extern "C" void port_sys_init(void)
{
    unsigned cpu;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
    cpu &= 3U;

    if (cpu == 0) {
        gic::gic.init();
    } else {
        gic::gic.init_cpu_interface();
    }

    timer_arm::init();
    timer_arm::start_1ms();

    gic::gic.enable_irq(gic::irq::PHYSICAL_TIMER);
    gic::gic.set_priority(gic::irq::PHYSICAL_TIMER, 0xA0);
    gic::gic.enable_irq(gic::irq::PHYSICAL_TIMER_NS);
    gic::gic.set_priority(gic::irq::PHYSICAL_TIMER_NS, 0xA0);
}

extern "C" unsigned port_cpu_id(void)
{
    unsigned mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3U;
}

extern "C" void port_smp_ipi(unsigned cpu)
{
    if (cpu < OS_NCPU) {
        gic::gic.send_sgi(1u << cpu, OS_SMP_IPI_SGI);
    }
}
