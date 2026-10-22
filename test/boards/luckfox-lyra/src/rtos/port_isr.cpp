/*
 * port_isr.cpp - RK3506 (GIC-400) interrupt dispatch for the AArch32 SMP port.
 *
 * The board's half of src/rtos/os-core.cpp: which interrupt fired, how it is
 * acknowledged, and the sanity check on the stack pointer a switch is about
 * to land on. Everything else in the port's scheduler is board-neutral and
 * lives in the shared file.
 *
 * Unlike the BCM2837, the RK3506 has a GIC: the taken interrupt is read from
 * the CPU interface's IAR and every one of them -- timer PPI, reschedule SGI
 * and any SPI -- has to be given back an EOI with the same value.
 */

#include <cstdint>
#include <cmsis-plus/rtos/os.h>
#include <port_ctx.hpp>
#include <timer_arm.hpp>
#include <gic.hpp>
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

// Weak default for external (non-timer, non-IPI) IRQs — override per project.
// The argument is the raw IAR, so an override can recover both the interrupt
// id and, for an SGI, the core that sent it.
extern "C" [[gnu::weak]] void port_external_irq_handler(uint32_t iar)
{
    (void)iar;
}

// C-style ISR handler called from assembly irq_handler.
extern "C" uint32_t * port_isr_handler(uint32_t *current_sp)
{
  unsigned cpu = port_cpu_id();
  _in_isr[cpu] = true;

  const uint32_t iar    = gic::gic.acknowledge_raw();
  const uint32_t irq_id = iar & 0x3FF;

  if (irq_id == gic::irq::PHYSICAL_TIMER || irq_id == gic::irq::PHYSICAL_TIMER_NS)
    {
      // Reload this core's private ARM Generic Timer for the next period.
      // The period is frequency()/1000/OS_SYSTICK_DIV: 1 ms by default, or
      // 0.5 ms when a test builds with -DOS_SYSTICK_DIV=2. The µOS++ sysclock
      // still advances at 1 ms: core 0 calls os_systick_handler() only on
      // every OS_SYSTICK_DIV-th interrupt (preemption stays at the finer rate
      // on every core).
      timer_arm::rearm_period();  // absolute: no drift

      gic::gic.end_of_interrupt(iar);

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
  else if (irq_id == OS_SMP_IPI_SGI)
    {
      gic::gic.end_of_interrupt(iar);
      _port_ctx_pending[cpu] = 1;
    }
  else
    {
      port_external_irq_handler(iar);
      gic::gic.end_of_interrupt(iar);
    }

  uint32_t *ret_sp = current_sp;
  if (_port_ctx_pending[cpu])
    {
      // Do not switch while this core holds the kernel spinlock: the owner
      // and depth would be stranded on the outgoing thread's stack and the
      // next acquirer would spin forever. Deferring to the next tick is the
      // same rule the shared reschedule() applies.
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
