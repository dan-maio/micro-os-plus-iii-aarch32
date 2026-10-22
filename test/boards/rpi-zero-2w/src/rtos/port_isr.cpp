/*
 * port_isr.cpp - BCM2837 interrupt dispatch for the AArch32 SMP port.
 *
 * The board's half of src/rtos/os-core.cpp: which interrupt fired, how it is
 * acknowledged, and the sanity check on the stack pointer a switch is about
 * to land on. Everything else in the port's scheduler is board-neutral and
 * lives in the shared file.
 */

#include <cstdint>
#include <cmsis-plus/rtos/os.h>
#include <port_ctx.hpp>
#include <timer_arm.hpp>
#include <bcm_irq.hpp>
#include <uart.hpp>
#include <smp.hpp>

extern "C" unsigned port_cpu_id (void);
extern "C" void* port_switch_stacks (void* sp);
extern "C" void os_systick_handler (void);

extern "C" {
extern volatile bool _in_isr[OS_NCPU];
extern volatile uint32_t _port_core_alive[OS_NCPU];
}

// The pending flags live in the scheduler namespace (SMP port contract), so
// this ISR consults the same object the shared reschedule() writes. A
// C-linkage copy would be a second array nobody else touches.
using os::rtos::port::scheduler::_port_ctx_pending;

// Validate context frame helper (for debugging/safety check)
static void validate_context(uint32_t *new_sp, uint32_t *old_sp)
{
  if (new_sp < (uint32_t*)PORT_RAM_BASE || new_sp >= (uint32_t*)PORT_RAM_END)
    {
      __asm__ volatile("cpsid if" ::: "memory");
      uart::uart1 << "\n!!! CORRUPT STACK POINTER !!!\n";
      uart::uart1 << "  new_sp=0x"; uart::uart1.print_hex((uint32_t)new_sp, 8);
      uart::uart1 << "  old_sp=0x"; uart::uart1.print_hex((uint32_t)old_sp, 8);
      uart::uart1 << "\n";
      while (1) __asm__ volatile("wfi");
    }

  uint32_t saved_cpsr = new_sp[ctx_cpsr_word]; /* CPSR word inside ctx_t */
  uint32_t mode = saved_cpsr & 0x1F;

  if (mode == 0x13)
    return;  // Valid SVC mode

  // Halt on corrupt context
  __asm__ volatile("cpsid if" ::: "memory");
  uart::uart1 << "\n!!! CORRUPT CONTEXT FRAME !!!\n";
  uart::uart1 << "  new_sp=0x"; uart::uart1.print_hex((uint32_t)new_sp, 8);
  uart::uart1 << "  old_sp=0x"; uart::uart1.print_hex((uint32_t)old_sp, 8);
  uart::uart1 << "  saved_cpsr=0x"; uart::uart1.print_hex(saved_cpsr, 8);
  uart::uart1 << "\n";
  while (1) __asm__ volatile("wfi");
}

// Weak default for external (non-timer, non-IPI) IRQs — override per project
extern "C" [[gnu::weak]] void port_external_irq_handler(uint32_t source)
{
    (void)source;
}

#if defined(UOS_CMSIS_OS_VALIDATOR)
extern "C" void ARM_CMSIS_VALIDATOR_IRQHandler (void);
#endif

// C-style ISR handler called from assembly irq_handler.
//
// BCM2837 has no GIC: the taken IRQ is identified from the per-core local
// "IRQ source" register. Two sources drive the scheduler — the generic-timer
// PPI (CNTPNSIRQ) and mailbox 0 (the reschedule IPI). The timer is re-armed by
// writing CNTP_TVAL; the mailbox is cleared via the mailbox read/clear reg.
extern "C" uint32_t * port_isr_handler(uint32_t *current_sp)
{
  unsigned cpu    = port_cpu_id();
  _in_isr[cpu] = true;

  uint32_t source = bcm_irq::source(cpu);

  if (source & bcm::local::SRC_CNTPNSIRQ)
    {
      // Reload this core's private ARM Generic Timer for the next period.
      // The period is frequency()/1000/OS_SYSTICK_DIV: 1 ms by default, or
      // 0.5 ms when a test builds with -DOS_SYSTICK_DIV=2. The µOS++ sysclock
      // still advances at 1 ms: core 0 calls os_systick_handler() only on
      // every OS_SYSTICK_DIV-th interrupt (preemption stays at the finer rate
      // on every core).
      timer_arm::rearm_period();  // absolute: no drift

      _port_core_alive[cpu] = 1;   // this core is up and scheduling

      // Core 0 ticks the global clock once per logical (1 ms) tick.
      if (cpu == 0)
        {
          static unsigned systick_phase = 0;
          if (systick_phase == 0u)
            {
              os_systick_handler();
            }
          systick_phase = (systick_phase + 1u) % OS_SYSTICK_DIV;
        }

      _port_ctx_pending[cpu] = 1;
    }
  if (source & bcm::local::SRC_MBOX0)
    {
      bcm_irq::ack_ipi(cpu);
      _port_ctx_pending[cpu] = 1;
    }
#if defined(UOS_CMSIS_OS_VALIDATOR)
  // Mailbox 1 stands in for the validator's NVIC IRQ 0 (NVIC_* in
  // test/rpi-zero-2w/cmsis-os-validator/harness-suite.cpp). Only the
  // validator's image defines UOS_CMSIS_OS_VALIDATOR (tests.cmake).
  if (source & (1u << 5))
    {
      bcm::mmio_write(bcm::local::MBOX_RDCLR0 + 0x10u * cpu + 4u * 1, ~0u);
      bcm::dsb();
      ARM_CMSIS_VALIDATOR_IRQHandler();
    }
#endif
  // A GPU interrupt must be dispatched even when this same handler entry also
  // carries the timer tick or the reschedule IPI. The legacy BCM2837 IRQ is
  // level-triggered, so the old "only if nothing else is pending" test did not
  // LOSE the interrupt — but it could defer servicing it by a whole tick, which
  // stalls a bulk USB transfer and is invisible in a trace.
  if (source & bcm::local::SRC_GPU)
    {
      port_external_irq_handler(source);
    }

  uint32_t *ret_sp = current_sp;
  if (_port_ctx_pending[cpu])
    {
      if (!os::rtos::scheduler::locked () && os::rtos::port::scheduler::_smp_klock.owner != cpu)
        {
          _port_ctx_pending[cpu] = 0;
          uint32_t *new_sp = (uint32_t *)port_switch_stacks(current_sp);
          validate_context(new_sp, current_sp);
          ret_sp = new_sp;
        }
    }

  _in_isr[cpu] = false;
  return ret_sp;
}
