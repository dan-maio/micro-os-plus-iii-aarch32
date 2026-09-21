#pragma once

#include <cstdint>
#include <cmsis-plus/rtos/port/os-decls.h>

// The DRAM window this board's images live in. The port checks a stack
// pointer against it before switching to it, so a frame built from a wild
// pointer stops here instead of somewhere unrecognisable later.
//
// No integer suffix: context_switch.cpp pastes these straight into an
// `ldr rN, =...` and the assembler will not take a 'u'.
#define PORT_RAM_BASE 0x00008000
#define PORT_RAM_END  0x20000000

extern "C" {

// Per-core join/heartbeat status (0 = not started, 3 = scheduler entered).
extern volatile std::uint32_t g_core_stage[OS_NCPU];

// Secondary-core entry points (asm coreN_entry -> these).
void boot_core1();
void boot_core2();
void boot_core3();

// asm per-core secondary trampolines (set banked stacks + MMU, call boot_coreN).
void core1_entry(void);
void core2_entry(void);
void core3_entry(void);

// Secondary launch spin-table (indexed by core id).
extern volatile std::uint32_t __smp_spin[OS_NCPU];

}

namespace smp {

// Release cores 1..3: publish each core's entry into __smp_spin + SEV (wakes
// QEMU cores already parked in secondary_park), and poke the BCM2837
// mailbox-3 spin-table (releases armstub-held cores on real hardware).
void start_secondary_cores();

}

// Per-project: create the per-core idle threads for cores 1..3.
extern "C" void smp_install_boot_threads(void);
