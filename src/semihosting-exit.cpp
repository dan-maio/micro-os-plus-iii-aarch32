/**
 * @file semihosting-exit.cpp
 * @brief Strong _Exit() for the AArch32 port.
 *
 * The kernel's _Exit() is weak and, on this bare-metal port, would reset or
 * idle. This strong definition terminates the run through the port's
 * semihosting SYS_EXIT (see include/semihosting.hpp), so returning from
 * os_main() -- i.e. std::exit(code) -- ends the run with a real exit code:
 * 0 on success under QEMU, and the ADP_Stopped reason under OpenOCD.
 *
 * The trap and the exit parameter are chosen by the build variant (QEMU vs
 * hardware) inside semihosting.hpp.
 */
#include <semihosting.hpp>

extern "C" void _Exit (int code) __attribute__ ((noreturn));

extern "C" void
_Exit (int code)
{
#if defined(SEMIHOST)
  if (code == 0)
    {
      semihosting::exit_success ();
    }
  else
    {
      semihosting::exit_failure ();
    }
#else
  (void)code;
#endif
  for (;;)
    {
    }
}
