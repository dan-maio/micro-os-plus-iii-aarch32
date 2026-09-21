#pragma once

#include <cstdint>

#include <cmsis-plus/rtos/port/os-decls.h>

// The DRAM window this board's images live in -- see linker.ld (hardware) and
// linker-qemu-virt.ld (the emulated runs, where `virt` puts DRAM at
// 0x4000_0000). The port checks a stack pointer against it before switching
// to it, so a frame built from a wild pointer stops here instead of somewhere
// unrecognisable later.
#if defined(QEMU_BUILD)
#define PORT_RAM_BASE 0x40000000u
#define PORT_RAM_END  0x48000000u
#else
#define PORT_RAM_BASE 0x00200000u
#define PORT_RAM_END  0x08000000u
#endif

extern "C" {

extern volatile std::uint32_t g_core_stage[OS_NCPU];
extern volatile std::uint32_t g_core1_heartbeat;
extern volatile std::uint32_t g_core2_heartbeat;

void boot_core1();
void boot_core2();

}

namespace smp {

void start_secondary_cores();

}

extern "C" void smp_install_boot_threads(void);
