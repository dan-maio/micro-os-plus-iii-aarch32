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
  //
  // The trap AND the SYS_EXIT parameter differ between the emulator and the
  // debugger:
  //
  //   QEMU (QEMU_BUILD)  the classic Angel trap, SVC 0x123456, and QEMU takes
  //                      the exit reason as a VALUE in r1 (0x20026 on success).
  //   hardware (HW_BUILD) the trap is the debugger's: HLT #0xF000 for the Pi
  //                      (OpenOCD `aarch64` target) or SVC for the Lyra, and
  //                      OpenOCD takes the reason as a POINTER to the reason
  //                      word.
  //
  // Getting the QEMU case wrong is not fatal to run-qemu.sh (it reads the
  // RESULT line), but it makes QEMU return exit code 1, which a CTest-style
  // runner would report as a failure.
#if defined(QEMU_BUILD)
#define UOS_SEMIHOST_TRAP ".arm\n svc 0x123456\n"       /* Angel, QEMU */
#define UOS_SEMIHOST_EXIT_BY_VALUE 1
#elif defined(SEMIHOST_TRAP_HLT)
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

  // SYS_EXIT (0x18): stop the run and report a status to the host.
  //
  // QEMU wants the reason as a value in r1; OpenOCD wants a pointer to the
  // reason word. See UOS_SEMIHOST_EXIT_BY_VALUE above.
  inline void
  exit (int code, int subcode = 0) noexcept
  {
    register int r0 asm ("r0") = 0x18;
#if defined(UOS_SEMIHOST_EXIT_BY_VALUE)
    register int r1 asm ("r1") = code;
    asm volatile (
        UOS_SEMIHOST_TRAP
        : "+r" (r0)
        : "r" (r1)
        : "r2", "r3", "ip", "lr", "memory", "cc");
#else
    volatile uint32_t block[2] = { (uint32_t)code, (uint32_t)subcode };
    register volatile const uint32_t* r1 asm ("r1") = block;
    asm volatile (
        UOS_SEMIHOST_TRAP
        : "+r" (r0)
        : "r" (r1)
        : "r2", "r3", "ip", "lr", "memory", "cc");
#endif
  }

  inline void
  exit_success () noexcept
  {
    constexpr int adp_stopped_applicationexit = 0x20026;
    exit (adp_stopped_applicationexit);
  }

  // Same reason/status shape as the AArch64 port: ApplicationExit with a
  // non-zero status, rather than a bare InternalError reason. On the
  // OpenOCD path this reaches the debugger as the {reason, status} block the
  // strong _Exit() already builds; on QEMU the status word is not transmitted
  // (UOS_SEMIHOST_EXIT_BY_VALUE), and the verdict is still the RESULT line.
  inline void
  exit_failure () noexcept
  {
    constexpr int adp_stopped_applicationexit = 0x20026;
    exit (adp_stopped_applicationexit, 1);
  }

  // The verdict-and-stop used by a hardware test.
  //
  // A QEMU run has a process status: the test returns from os_main(), the
  // strong _Exit() (src/semihosting-exit.cpp) reports through SYS_EXIT, and
  // CTest reads the exit code. A hardware run under OpenOCD has no process
  // status -- the probe is a debugger, not a parent -- so the verdict is the
  // "RESULT: PASS|FAIL" line the runners grep, and SYS_EXIT is what STOPS the
  // run instead of leaving the core spinning until the runner times out.
  //
  // Printing the line on the semihosting channel (not the UART) is what makes
  // it reach the OpenOCD log, and calling exit_*() is what makes the strong
  // _Exit() path real for a test that would otherwise loop forever.
  [[noreturn]] inline void
  report_result (bool pass) noexcept
  {
    if (pass)
      {
        write_str ("\nRESULT: PASS\n");
        exit_success ();
      }
    else
      {
        write_str ("\nRESULT: FAIL\n");
        exit_failure ();
      }
    for (;;)
      {
      }
  }
} // namespace semihosting
