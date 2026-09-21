/*
 * os-core.cpp - the port's half of the µOS++ III SMP scheduler, AArch32.
 *
 * Shared by every board this port builds for. The interrupt-controller half
 * -- which interrupt fired, how it is acknowledged -- is the one thing that
 * differs between a BCM2837 (per-core local IRQ source register, no GIC) and
 * an RK3506 (GIC-400), and it lives in boards/<board>/src/rtos/port_isr.cpp.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

#include <string.h>
#include <cassert>
#include <cstddef>
#include <cmsis-plus/rtos/os.h>
// micro-os-plus-iii `develop` no longer pulls the C-ABI declarations in from
// the C++ <os.h>, so include them explicitly for os_thread_t (used below as
// sized storage for the boot/fake thread contexts).
#include <cmsis-plus/rtos/os-c-decls.h>
#include <port_ctx.hpp>
#include <timer_arm.hpp>
#include <uart.hpp>

// Forward declarations of reference functions
extern "C" void port_sys_init(void);
extern "C" void port_ctx_switchHandler(void);
extern "C" unsigned port_cpu_id(void);

extern os::rtos::thread* os_idle_thread;

// Context switch pending flags, one per core
extern "C" {
volatile unsigned _port_ctx_pending[OS_NCPU] = {};
volatile bool _in_isr[OS_NCPU] = {};

/* Per-core "alive" flag: set on this core's first 1ms timer tick, i.e. once
 * it has completed secondary bring-up and is scheduling with interrupts on.
 * smp::start_secondary_cores() waits on it to release cores one at a time
 * (avoids simultaneous first-switch idle-thread claim races). */
volatile uint32_t _port_core_alive[OS_NCPU] = {};

/* SMP deferred publish slots (per CPU). The scheduler stages
 * {&old_thread->stack_ptr, frame_sp} here; the asm restore path
 * (irq_handler / port_ctx_switchHandler) performs the actual store
 * only AFTER this CPU's SP has left the outgoing thread's stack.
 * Publishing earlier would let another core run the outgoing thread
 * on the very stack our remaining kernel frames still occupy. */
volatile uint32_t _smp_pub_addr[OS_NCPU] = {};
volatile uint32_t _smp_pub_val[OS_NCPU]  = {};
}

static_assert(sizeof(os_thread_t) == sizeof(os::rtos::thread),
              "CRITICAL ERROR: Size mismatch between C os_thread_t and C++ rtos::thread!");

// Reopen and declare scheduler member
namespace os
{
  namespace rtos
  {
    namespace scheduler
    {
      extern thread* os_idle_thread_core[OS_NCPU];
    }
  }
}

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace scheduler
      {
        volatile state_t lock_state[OS_NCPU];

        result_t
        initialize (void)
        {
          __asm__ volatile("cpsid i" ::: "memory");
          return result::ok;
        }

        void
        start (void)
        {
          // Disable interrupts for safe setup
          __asm__ volatile("cpsid i" ::: "memory");

          // Initialize GIC and ARM Generic Timer
          port_sys_init();

          // Set up a fake thread context to receive the initial boot context
          static os_thread_t fake_thread;
          memset (&fake_thread, 0, sizeof (os_thread_t));
          fake_thread.name = "fake_thread";
          rtos::thread* pth = reinterpret_cast<rtos::thread*> (&fake_thread);
          unsigned cpu0 = port_cpu_id();
          rtos::scheduler::current_thread_[cpu0] = pth;

          // Set lock_state to unlocked/init for all cores
          for (unsigned c = 0; c < OS_NCPU; ++c)
            lock_state[c] = state::init;

          // Initialize Core 0's idle thread in the SMP array
          os::rtos::scheduler::os_idle_thread_core[0] = ::os_idle_thread;

          // Call reschedule to trigger the initial switch to main thread
          scheduler::reschedule ();

          // Enable interrupts and wait (though reschedule will switch away immediately)
          __asm__ volatile("cpsie i" ::: "memory");
          for (;;) {
            __asm__ volatile("wfi");
          }
        }

        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          if (state == state::locked)
            {
              __asm__ volatile("cpsid i" ::: "memory");
              unsigned cpu = port_cpu_id();
              state_t tmp = lock_state[cpu];
              if (tmp != state::locked)
                {
                  if (_smp_klock.owner != cpu)
                    {
                      _smp_klock_raw_acquire();
                      _smp_klock.owner = cpu;
                    }
                  _smp_klock.depth = _smp_klock.depth + 1;
                  lock_state[cpu] = state::locked;
                }
              return tmp;
            }
          else
            {
              unsigned cpu = port_cpu_id();
              state_t tmp = lock_state[cpu];
              if (tmp != state::unlocked)
                {
                  lock_state[cpu] = state::unlocked;
                  if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
                    {
                      uint32_t d = _smp_klock.depth - 1;
                      _smp_klock.depth = d;
                      if (d == 0)
                        {
                          _smp_klock.owner = SMP_NO_OWNER;
                          _smp_klock_raw_release();
                        }
                    }
                  __asm__ volatile("cpsie i" ::: "memory");
                }
              return tmp;
            }
        }

        void
        reschedule (void)
        {
          unsigned cpu = port_cpu_id();
          if (rtos::scheduler::locked ()
              || (rtos::interrupts::in_handler_mode ()
                  && !rtos::scheduler::preemptive ()))
            {
              return;
            }


          // If this core still owns the kernel spinlock (resume_one() was
          // invoked from inside an interrupts::critical_section, as
          // message_queue::send/receive do), do NOT context-switch here:
          // switching threads while the lock is held would strand the owner
          // and depth on the outgoing thread's stack and deadlock the next
          // acquirer. Defer to the next reschedule/tick, which runs once the
          // caller's critical section has been released.
          if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
            {
              if (cpu < OS_NCPU)
                {
                  _port_ctx_pending[cpu] = 1;
                }
              return;
            }

          if (rtos::interrupts::in_handler_mode ())
            {
              if (cpu < OS_NCPU) {
                _port_ctx_pending[cpu] = 1;
              }
            }
          else
            {
              port_ctx_switchHandler ();
            }
        }

        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          unsigned cpu = port_cpu_id();
          {
            uint32_t tmp, fail;
            __asm__ volatile(
              "1:  ldrex   %0, [%2]      \n"
              "    cmp     %0, #0        \n"
              "    bne     1b            \n"
              "    mov     %0, #1        \n"
              "    strex   %1, %0, [%2]  \n"
              "    cmp     %1, #0        \n"
              "    bne     1b            \n"
              "    dmb                   \n"
              : "=&r"(tmp), "=&r"(fail)
              : "r"(&_smp_klock.lock)
              : "cc", "memory");
          }
          _smp_klock.owner = cpu;
          _smp_klock.depth = 1;


          rtos::thread* old_thread = rtos::scheduler::current_thread_[cpu];

          rtos::scheduler::internal_switch_threads ();

          rtos::thread* new_thread = rtos::scheduler::current_thread_[cpu];
          if (new_thread == nullptr) {
            __asm__ volatile("cpsid if" ::: "memory");
            uart::uart1 << "\n!!! ERROR: no ready thread and no idle thread on Core " << static_cast<int>(cpu) << " !!!\n";
            while (1) __asm__ volatile("wfi");
          }
          stack::element_t* out_sp;
          _smp_pub_addr[cpu] = 0;   /* no publish unless staged below */
          if (new_thread != old_thread) {
            /* Tripwire (debug, from stateos SMP_FINE smp_switch_check):
             * the claimed frame MUST lie inside the incoming thread's own
             * stack. A stale/foreign pointer here is the signature of a
             * publish/claim protocol violation - halt with evidence at
             * the source instead of crashing later on a garbage restore. */
            {
              class rtos::thread::stack& stk = new_thread->context_.stack ();
              char* lo = reinterpret_cast<char*> (stk.bottom ());
              char* hi = reinterpret_cast<char*> (stk.top ());
              char* p  = reinterpret_cast<char*> (
                  new_thread->context_.port_.stack_ptr);
              /* A valid saved frame always has CPSR mode = SVC (0x13) at
               * word 34 - catches stale-offset frames inside the right
               * stack, not just out-of-bounds pointers. */
              bool bad_bounds
                  = (lo != nullptr && hi > lo
                     && (p < lo
                         || p + static_cast<ptrdiff_t> (ctx_size_bytes) > hi));
              bool bad_frame
                  = (p != nullptr
                     && ((reinterpret_cast<uint32_t*> (p)[ctx_cpsr_word]
                          & 0x1FU)
                         != 0x13U));
              if (bad_bounds || bad_frame)
                {
                  __asm__ volatile("cpsid if" ::: "memory");
                  uart::uart1 << "\n!!! SMP CLAIM TRIPWIRE (core "
                              << static_cast<int>(cpu) << ") !!!\n  old=";
                  uart::uart1 << (old_thread->name () ? old_thread->name () : "?");
                  uart::uart1 << " new=";
                  uart::uart1 << (new_thread->name () ? new_thread->name () : "?");
                  uart::uart1 << "\n  claimed sp=0x";
                  uart::uart1.print_hex (reinterpret_cast<uint32_t> (p), 8);
                  uart::uart1 << " stack=[0x";
                  uart::uart1.print_hex (reinterpret_cast<uint32_t> (lo), 8);
                  uart::uart1 << ",0x";
                  uart::uart1.print_hex (reinterpret_cast<uint32_t> (hi), 8);
                  uart::uart1 << ")\n  new state=";
                  uart::uart1.print_hex (
                      static_cast<uint32_t> (new_thread->state ()), 2);
                  uart::uart1 << "\n";
                  while (1) __asm__ volatile("wfi");
                }
            }
            /* Stage the outgoing thread's frame publish; the asm restore
             * path stores it after SP has left old_thread's stack. */
            _smp_pub_val[cpu]  = reinterpret_cast<uint32_t>(sp);
            _smp_pub_addr[cpu] = reinterpret_cast<uint32_t>(
                &old_thread->context_.port_.stack_ptr);
            out_sp = new_thread->context_.port_.stack_ptr;
            /* Claim: new_thread's context is now live in this CPU's
             * registers; other cores must not pick it (see the
             * stack_ptr != nullptr guard in internal_switch_threads). */
            new_thread->context_.port_.stack_ptr = nullptr;
          } else {
            out_sp = sp;
          }

          /* Release the kernel lock. THE ORDER MATTERS: the recursive-lock
           * bookkeeping (owner/depth) must be cleared BEFORE the lock word,
           * which is exactly what every other release site does (clear
           * owner/depth, then _smp_klock_raw_release()).
           *
           * Storing lock=0 first opens a window of two instructions in which
           * another core spinning in _smp_klock_raw_acquire() wins the lock
           * and installs its own owner/depth (interrupts::critical_section::
           * enter()); the two stores below then wipe them. That core's
           * critical_section::exit() is guarded by
           * (owner == cpu && depth > 0), so it never clears the lock word,
           * and from then on every core spins forever in
           * _smp_klock_raw_acquire() -- a silent, total SMP freeze. */
          _smp_klock.depth = 0;
          _smp_klock.owner = SMP_NO_OWNER;
          __asm__ volatile("dmb" ::: "memory");
          _smp_klock.lock = 0;
          __asm__ volatile("dsb" ::: "memory");

          return out_sp;
        }

      } /* namespace scheduler */

      // Implement static member functions of class context
      void
      context::create (void* context, void* func, void* args)
      {
        class rtos::thread::context* th_ctx
            = static_cast<class rtos::thread::context*> (context);

        class rtos::thread::stack& stack = th_ctx->stack ();

        rtos::thread::stack::element_t* p = stack.top ();

        // Align stack top to 8 bytes
        p = reinterpret_cast<rtos::thread::stack::element_t*> (
            (reinterpret_cast<uintptr_t> (p)) & (~7ULL));

        // Reserve space for the context frame
        p -= (sizeof (ctx_t) / sizeof (rtos::thread::stack::element_t));

        // Align the frame to 8 bytes
        p = reinterpret_cast<rtos::thread::stack::element_t*> (
            (reinterpret_cast<uintptr_t> (p)) & (~7ULL));

        ctx_t* f = reinterpret_cast<ctx_t*> (p);

        // Clear VFP registers (both halves: d0-d15 and d16-d31)
        for (int i = 0; i < 32; ++i)
          {
            f->vfp[i] = 0;
            f->vfp_hi[i] = 0;
          }
        f->fpscr = 0;

        // CPSR: SVC mode (0x13), interrupts enabled
        f->cpsr = 0x00000013U;

        // PC is the function to run
        f->pc = reinterpret_cast<uintptr_t> (func);

        // R0 is the argument
        f->r[0] = reinterpret_cast<uintptr_t> (args);

        // Other registers to pattern
        for (int i = 1; i < 13; ++i)
          f->r[i] = 0xCCCCCCCC;

        f->lr = 0;

        // Store the stack pointer in the context
        th_ctx->port_.stack_ptr = reinterpret_cast<uint32_t*> (p);
      }

      // Implement static member functions of class clock_systick
      void
      clock_systick::start (void)
      {
        // Systick is handled by the ARM Generic Timer initialization
        // already triggered in scheduler::start() -> port_sys_init()
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

// C wrapper for assembly context switcher
extern "C" void* port_switch_stacks(void* sp)
{
  return os::rtos::port::scheduler::switch_stacks(static_cast<os::rtos::port::stack::element_t*>(sp));
}

// validate_context(), port_external_irq_handler() and port_isr_handler() are
// the only parts of this file that depend on the interrupt controller, so
// they live per board in boards/<board>/src/rtos/port_isr.cpp. Everything
// above and below is the same on every ARMv7-A board this port runs on.
extern "C" void os_systick_handler (void);

namespace os { namespace rtos { namespace scheduler {
  extern thread* os_idle_thread_core[OS_NCPU];
}}}

extern "C" void port_smp_secondary_start(void)
{
  __asm__ volatile("cpsid i" ::: "memory");

  unsigned cpu = port_cpu_id();

  // Initialize GIC CPU interface and local timer
  port_sys_init();

  // Set up a fake thread context to receive the initial boot context
  static os_thread_t fake_thread_secondary[OS_NCPU];
  memset (&fake_thread_secondary[cpu], 0, sizeof (os_thread_t));
  fake_thread_secondary[cpu].name = "fake_thread_sec";
  os::rtos::thread* pth = reinterpret_cast<os::rtos::thread*> (&fake_thread_secondary[cpu]);
  os::rtos::scheduler::current_thread_[cpu] = pth;

  // Call reschedule to trigger the initial switch to whatever thread is ready
  os::rtos::port::scheduler::reschedule ();

  __asm__ volatile("cpsie i" ::: "memory");
  for (;;) {
    __asm__ volatile("wfi");
  }
}

// Override µOS++ trace::write to output to UART1
namespace os
{
  namespace trace
  {
    ssize_t
    write (const void* buf, std::size_t nbyte)
    {
      const char* cbuf = reinterpret_cast<const char*>(buf);
      for (std::size_t i = 0; i < nbyte; ++i)
        {
          if (cbuf[i] == '\n')
            {
              uart::uart1.putc('\r');
            }
          uart::uart1.putc(cbuf[i]);
        }
      return static_cast<ssize_t>(nbyte);
    }
  }
}
