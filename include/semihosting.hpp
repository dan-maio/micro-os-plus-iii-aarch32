/**
 * @file  semihosting.hpp
 * @brief Minimal ARM semihosting console for the Raspberry Pi Zero 2W.
 *
 * Used ONLY for hardware (J-Link + OpenOCD) debug-in-RAM runs, so the UART
 * console output can also be captured on the host through OpenOCD's
 * semihosting channel ("arm semihosting enable"). Mirrored automatically by
 * uart.hpp when the build defines SEMIHOST (see hw.sh).
 *
 * Cortex-A53 in AArch32 under the OpenOCD "aarch64" debug wrapper: OpenOCD
 * expects the HLT #0xF000 semihosting breakpoint, NOT SVC 0x123456 (the
 * classic AArch32 trap), because the debug interface is AArch64.
 *
 *   HLT #0xF000  (AArch32 encoding) = 0xE10F0070
 *
 * When the binary is NOT run under a semihosting-enabled debugger every HLT
 * will halt the core forever, so a SEMIHOST build is only for hw.sh runs.
 */
#pragma once

#include <cstdint>

namespace semihosting
{
  // SYS_WRITEC (0x03): write the single char pointed to by r1.
  inline void
  write_char (char c) noexcept
  {
    register int r0 asm ("r0") = 0x03;
    register const char* r1 asm ("r1") = &c;
    asm volatile (
        ".arm\n"
        ".inst 0xE10F0070\n" // HLT #0xF000
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
        ".arm\n"
        ".inst 0xE10F0070\n" // HLT #0xF000
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
        ".arm\n"
        ".inst 0xE10F0070\n" // HLT #0xF000
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
