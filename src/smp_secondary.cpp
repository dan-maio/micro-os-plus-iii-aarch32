/*
 * smp_secondary.cpp - secondary-core entry glue. The asm coreN_entry sets up
 * this core's banked stacks + MMU, then calls boot_coreN, which records the
 * join stage and hands off to the shared SMP secondary start (GIC-free timer +
 * mailbox-IPI init, fake context, first reschedule).
 */
#include <cstdint>
#include "smp.hpp"

extern "C" void port_smp_secondary_start(void);

extern "C" void boot_core1()
{
    g_core_stage[1] = 3;
    port_smp_secondary_start();
}

extern "C" void boot_core2()
{
    g_core_stage[2] = 3;
    port_smp_secondary_start();
}

extern "C" void boot_core3()
{
    g_core_stage[3] = 3;
    port_smp_secondary_start();
}
