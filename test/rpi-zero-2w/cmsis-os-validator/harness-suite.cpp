// The harness suite "cmsis-os-validator".
//
// Its sources are not here: they live in the harness,
// tests/sources/cmsis-os-validator/, and are named by board_test_libs() in
// test/rpi-zero-2w/tests.cmake. This file exists because the port's builder
// builds one application per directory that has a *.cpp, and this directory
// is where the application's NAME comes from --
// test/rpi-zero-2w/cmsis-os-validator/ -- so that the platform registers
// cmsis-os-validator-qemu and cmsis-os-validator-hwd like every other test.
//
// It also provides the Cortex-A53 / BCM2837 native interrupt controller glue
// for the validator's interrupt tests: the validator raises NVIC IRQ 0, which
// here is BCM2837 Mailbox 1 (dispatched by src/rtos/port_isr.cpp).

#include <cstdlib>
#include <cstdint>

#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <semihosting.hpp>
#include <bcm_irq.hpp>

typedef int IRQn_Type;

extern "C" uint32_t SystemCoreClock;
uint32_t SystemCoreClock = 1000000000;

namespace
{
  // The mailbox that stands in for NVIC IRQ 0 (mailbox 0 is the IPI).
  constexpr unsigned kValidatorMbox = 1u;
} // namespace

extern "C" void
NVIC_EnableIRQ (IRQn_Type)
{
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      // Clear anything pending, then let Mailbox 1 interrupt this core.
      bcm::mmio_write (bcm::local::MBOX_RDCLR0 + 0x10u * c
                           + 4u * kValidatorMbox,
                       ~0u);
      uint32_t ctl = bcm::mmio_read (bcm::local::MBOX_IRQCNTL0 + 4u * c);
      bcm::mmio_write (bcm::local::MBOX_IRQCNTL0 + 4u * c,
                       ctl | (1u << kValidatorMbox));
    }
  bcm::dsb ();
}

extern "C" void
NVIC_DisableIRQ (IRQn_Type)
{
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      uint32_t ctl = bcm::mmio_read (bcm::local::MBOX_IRQCNTL0 + 4u * c);
      bcm::mmio_write (bcm::local::MBOX_IRQCNTL0 + 4u * c,
                       ctl & ~(1u << kValidatorMbox));
    }
  bcm::dsb ();
}

extern "C" void
NVIC_SetPendingIRQ (IRQn_Type)
{
  // The interrupt is taken by the core that raises it, as on a Cortex-M.
  unsigned c = bcm_irq::this_core ();
  bcm::mmio_write (bcm::local::MBOX_SET0 + 0x10u * c + 4u * kValidatorMbox,
                   1u);
  bcm::dsb ();
  bcm::sev ();
}

extern "C" int
__real_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[])
{
  // CMSIS-RTOS v1 assumes one CPU: a thread of higher priority preempts its
  // creator at once. The kernel's CMSIS wrapper pins every thread it creates
  // to core 0 (os-c-wrapper.cpp, osThreadCreate); the main thread, which runs
  // the validator, is not created there, so pin it too, and yield so that the
  // scheduler moves it to core 0 before the first test. Without it the main
  // thread keeps running on another core in parallel with the threads it has
  // just created.
  os::rtos::this_thread::thread ().cpu_affinity (1u << 0);
  os::rtos::this_thread::yield ();

  int code = __real_os_main (argc, argv);

  // The UART terminal sees the verdict...
  uart::uart1.puts_uart (code == 0 ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");

  // ...and OpenOCD, which is what hw.sh greps, sees it on the semihosting
  // channel, because report_result() writes there and then stops the run with
  // the semihosting SYS_EXIT -- exactly what the port's own hw tests do.
  //
  // Do NOT return to the trampoline's std::exit(): on this SMP port that runs
  // the atexit handlers and the C++ static destructors while the suite still
  // has threads and mutexes alive (see mutex-stress/harness-suite.cpp).
#if defined(SEMIHOST)
  semihosting::report_result (code == 0);
#else
  std::exit (code);
#endif
  return code;
}
