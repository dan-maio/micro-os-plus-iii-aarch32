#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/diag/trace.h>
#include <timer_arm.hpp>

#ifdef __cplusplus

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace scheduler
      {
        inline void __attribute__ ((always_inline))
        greeting (void)
        {
          // PORT_GREETING names the board; the port's CMakeLists defines it
          // per board. It used to be this one string, so every board this
          // port grew announced itself as a Raspberry Pi.
          trace::printf ("Scheduler: " PORT_GREETING ", %d CPU(s), 1ms tick, LDREX lock\n",
                         (int) OS_NCPU);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        lock (void)
        {
          return locked (state::locked);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        unlock (void)
        {
          return locked (state::unlocked);
        }

        inline unsigned __attribute__ ((always_inline))
        port_cpu_id_inline (void)
        {
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          return cpu & 3U;
        }

        inline bool __attribute__ ((always_inline))
        locked (void)
        {
          uint32_t cpsr;
          __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr) :: "memory");
          bool res = (lock_state[port_cpu_id_inline()] != state::unlocked);
          if (!(cpsr & 0x80U))
            __asm__ volatile("cpsie i" ::: "memory");
          return res;
        }

        inline void __attribute__ ((always_inline))
        wait_for_interrupt (void)
        {
          __asm__ volatile("dsb" ::: "memory");
          __asm__ volatile("wfi");
        }

        /* SMP: acquire scheduler lock word (LDREX/STREX) */
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_acquire (void)
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

        /* SMP: release scheduler lock word */
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_release (void)
        {
          __asm__ volatile("dmb" ::: "memory");
          _smp_klock.lock = 0;
          __asm__ volatile("dsb" ::: "memory");
        }

        /* SMP: enter kernel critical section (mask IRQs + take scheduler lock) */
        inline void __attribute__ ((always_inline))
        port_set_lock (void)
        {
          __asm__ volatile("cpsid i" ::: "memory");
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
          if (_smp_klock.owner != cpu)
          {
            _smp_klock_raw_acquire();
            _smp_klock.owner = cpu;
          }
          _smp_klock.depth = _smp_klock.depth + 1;
        }

        /* SMP: leave kernel critical section */
        inline void __attribute__ ((always_inline))
        port_put_lock (uint32_t lck)
        {
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
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
          if (lck & 0x80U)
            __asm__ volatile("cpsid i" ::: "memory");
          else
            __asm__ volatile("cpsie i" ::: "memory");
        }

        /* SMP: full kernel-lock clear (at task body entry) */
        inline void __attribute__ ((always_inline))
        port_clr_lock (void)
        {
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
          if (_smp_klock.owner == cpu)
          {
            _smp_klock.depth = 0;
            _smp_klock.owner = SMP_NO_OWNER;
            _smp_klock_raw_release();
          }
          __asm__ volatile("cpsie i" ::: "memory");
        }

        /* SMP: migrate lock depth at context switch */
        inline void __attribute__ ((always_inline))
        port_smp_migrate (unsigned depth)
        {
          if (depth == 0)
          {
            _smp_klock.depth = 0;
            _smp_klock.owner = SMP_NO_OWNER;
            _smp_klock_raw_release();
          }
          else
          {
            _smp_klock.depth = depth;
          }
        }

        /* SMP: get current scheduler lock depth */
        inline unsigned __attribute__ ((always_inline))
        port_smp_depth (void)
        {
          return _smp_klock.depth;
        }

        /* SMP: acquire timer leaf lock */
        inline void __attribute__ ((always_inline))
        port_tmr_lock (void)
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
            : "r"(&_smp_tlock.lock)
            : "cc", "memory");
        }

        /* SMP: release timer leaf lock */
        inline void __attribute__ ((always_inline))
        port_tmr_unlock (void)
        {
          __asm__ volatile("dmb" ::: "memory");
          _smp_tlock.lock = 0;
          __asm__ volatile("dsb" ::: "memory");
        }
      }

      namespace interrupts
      {
        extern "C" volatile bool _in_isr[OS_NCPU];

        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
          if (cpu < OS_NCPU && _in_isr[cpu])
            {
              return true;
            }
          uint32_t cpsr;
          __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
          uint32_t mode = cpsr & 0x1FU;
          return (mode == 0x12U) || (mode == 0x11U);
        }

        inline bool
        is_priority_valid (void)
        {
          return true;
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        critical_section::enter (void)
        {
          uint32_t cpsr;
          __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
          __asm__ volatile("cpsid i" ::: "memory");
          
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
          if (scheduler::_smp_klock.owner != cpu)
          {
            scheduler::_smp_klock_raw_acquire();
            scheduler::_smp_klock.owner = cpu;
          }
          scheduler::_smp_klock.depth = scheduler::_smp_klock.depth + 1;
          
          return cpsr;
        }

        inline void __attribute__ ((always_inline))
        critical_section::exit (rtos::interrupts::state_t state)
        {
          unsigned cpu;
          __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
          cpu &= 3U;
          if (scheduler::_smp_klock.owner == cpu && scheduler::_smp_klock.depth > 0)
          {
            uint32_t d = scheduler::_smp_klock.depth - 1;
            scheduler::_smp_klock.depth = d;
            if (d == 0)
            {
              scheduler::_smp_klock.owner = SMP_NO_OWNER;
              scheduler::_smp_klock_raw_release();
            }
          }
          if (state & 0x80U) {
            __asm__ volatile("cpsid i" ::: "memory");
          } else {
            __asm__ volatile("cpsie i" ::: "memory");
          }
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        uncritical_section::enter (void)
        {
          uint32_t cpsr;
          __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
          __asm__ volatile("cpsie i" ::: "memory");
          return cpsr;
        }

        inline void __attribute__ ((always_inline))
        uncritical_section::exit (rtos::interrupts::state_t state)
        {
          if (state & 0x80U) {
            __asm__ volatile("cpsid i" ::: "memory");
          } else {
            __asm__ volatile("cpsie i" ::: "memory");
          }
        }
      }

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
        }
      }

      inline void __attribute__ ((always_inline))
      clock_highres::start (void)
      {
      }

      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return true;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        return timer_arm::get_count ();
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::input_clock_frequency_hz (void)
      {
        return timer_arm::get_freq();
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_per_tick (void)
      {
        return timer_arm::get_freq() / 1000;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_since_tick (void)
      {
        uint32_t load_value = cycles_per_tick();
        uint32_t val = timer_arm::get_tval();
        if (val > load_value) {
          return 0;
        }
        return load_value - val;
      }

      /* SMP: 32-bit compare-and-swap (LDREX/STREX) */
      inline bool __attribute__ ((always_inline))
      port_cas32 (volatile uint32_t *ptr,
                   uint32_t expected, uint32_t desired)
      {
        uint32_t old, fail;
        __asm__ volatile(
          "    ldrex   %0, [%2]      \n"
          "    cmp     %0, %3        \n"
          "    bne     1f            \n"
          "    strex   %1, %4, [%2]  \n"
          "    cmp     %1, #0        \n"
          "    bne     2f            \n"
          "    dmb                   \n"
          "    b       3f            \n"
          "1:  clrex                 \n"
          "2:  mov     %1, #1        \n"
          "    b       4f            \n"
          "3:  mov     %1, #0        \n"
          "4:                        \n"
          : "=&r"(old), "=&r"(fail)
          : "r"(ptr), "r"(expected), "r"(desired)
          : "cc", "memory");
        return fail == 0 && old == expected;
      }

    }
  }
}

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
