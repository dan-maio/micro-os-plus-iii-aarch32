/**
 * @file  semihosting.hpp
 * @brief Minimal ARM semihosting console for the AArch32 port.
 *
 * Used ONLY for hardware (OpenOCD) debug-in-RAM runs, so the UART console
 * output can also be captured on the host through OpenOCD's semihosting
 * channel ("arm semihosting enable"). Mirrored automatically by uart.hpp when
 * the build defines SEMIHOST (see hw.sh).
 *
 * THE TRAP INSTRUCTION IS A PROPERTY OF THE DEBUGGER, NOT OF THE CPU.
 * OpenOCD recognises whichever one the *target type* it is driving expects:
 *
 *   cortex_a  (ARMv7-A)   SVC 0x123456   the classic AArch32 Angel trap
 *   aarch64               HLT #0xF000    the ARMv8 encoding, 0xE10F0070
 *
 * SEMIHOST_TRAP_HLT selects the second, and the port's CMakeLists defines it
 * per board:
 *
 *   Raspberry Pi Zero 2 W / Pi 3 B -- HLT. The Cortex-A53 are declared as
 *     `aarch64` targets even for this 32-bit port, because the ARMv7
 *     `cortex_a` target does not support the -cti option the BCM2837 needs
 *     (boards/rpi-zero-2w/bcm2837-aarch32.cfg says so in its own header).
 *
 *   Luckfox Lyra B -- SVC. The Cortex-A7 are real `cortex_a` targets.
 *
 * Getting this wrong is not a missing console. `HLT #0xF000` has no encoding
 * on ARMv7-A: the word 0xE10F0070 is UNDEFINED there, so the first character
 * printed raises an Undefined Instruction exception -- and if the fault
 * handler prints through this same path, it raises another one, and the board
 * loops forever emitting the first line of a fault dump.
 *
 * When the binary is NOT run under a semihosting-enabled debugger the trap
 * halts the core (HLT) or vectors to an unhandled SVC, so a SEMIHOST build is
 * only for hw.sh runs.
 */
#pragma once

#include <cstdint>

namespace semihosting
{
  // The trap, spelled once. Both forms leave r0/r1 as the Angel ABI wants
  // them and clobber the same registers.
#if defined(SEMIHOST_TRAP_HLT)
#define UOS_SEMIHOST_TRAP ".arm\n .inst 0xE10F0070\n"   /* HLT #0xF000 */
#else
#define UOS_SEMIHOST_TRAP ".arm\n svc 0x123456\n"       /* Angel, ARMv7-A */
#endif

  // SYS_WRITEC (0x03): write the single char pointed to by r1.
  inline void
  write_char (char c) noexcept
  {
    register int r0 asm ("r0") = 0x03;
    register const char* r1 asm ("r1") = &c;
    asm volatile (
        UOS_SEMIHOST_TRAP
        : "+r" (r0)
        : "r" (r1)
        : "r2", "r3", "ip", "lr", "memory", "cc");
  }

  // SYS_WRITE0 (0x04): write the NUL-terminated string pointed to by r1.
  inline void
  write_str (const char* s) noexcept
  {
    register int r0 asm ("r0") = 0x04;
    register const char* r1 asm ("r1") = s;
    asm volatile (
        UOS_SEMIHOST_TRAP
        : "+r" (r0)
        : "r" (r1)
        : "r2", "r3", "ip", "lr", "memory", "cc");
  }

  // SYS_EXIT (0x18): stop the run and report a status to the host. r1 must
  // point to the ADP_Stopped reason block (a single 32-bit code): 0x20026 is
  // ADP_Stopped_ApplicationExit (success); anything else is treated as an
  // error by the debugger / host tooling.
  inline void
  exit (int code) noexcept
  {
    register int r0 asm ("r0") = 0x18;
    register const int* r1 asm ("r1") = &code;
    asm volatile (
        UOS_SEMIHOST_TRAP
        : "+r" (r0)
        : "r" (r1)
        : "r2", "r3", "ip", "lr", "memory", "cc");
  }

  inline void
  exit_success () noexcept
  {
    constexpr int adp_stopped_applicationexit = 0x20026;
    exit (adp_stopped_applicationexit);
  }

  inline void
  exit_failure () noexcept
  {
    constexpr int adp_stopped_internalerror = 0x20023;
    exit (adp_stopped_internalerror);
  }
} // namespace semihosting
