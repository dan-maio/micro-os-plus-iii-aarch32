/**
 * @file timer_arm.hpp
 * @brief ARM Generic Timer, ARMv7-A
 * @details Provides inline functions for accessing ARM Generic Timer registers
 *          including frequency control, timer value, and control register.
 * @note Uses ARM CP15 coprocessor registers for timer access.
 *
 * Every accessor here is a CP15 instruction, which is a fact about ARMv7-A
 * and not about any board, so there is one copy. What differs per board is
 * how init() decides the counter's real rate -- the Pi has to measure it
 * because its CNTFRQ lies -- and that lives in
 * boards/<board>/src/timer_arm.cpp.
 */

#ifndef TIMER_ARM_HPP
#define TIMER_ARM_HPP

#include <cstdint>

// Timer-interrupt pre-scale vs the 1 ms logical tick. Default 1 = the ARM
// generic timer interrupts at 1 ms. A test may define OS_SYSTICK_DIV to 2
// (via -D) for a 0.5 ms interrupt/preemption period; the port's ISR then
// advances os_systick_handler() only on every DIV-th IRQ, so the µOS++
// sysclock stays at 1 ms while preemption is twice as frequent.
#ifndef OS_SYSTICK_DIV
#define OS_SYSTICK_DIV 1
#endif

namespace timer_arm {

inline constexpr std::uint32_t TIMER_ARM_FREQ = 1'000'000;

// ============================================================================
// Timer Register Access Functions
// ============================================================================

inline void set_freq(std::uint32_t freq) noexcept {
    __asm__ volatile("mcr p15, 0, %0, c14, c0, 0" : : "r"(freq) : "memory");
}

inline std::uint32_t get_freq() noexcept {
    std::uint32_t freq;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(freq) : : "memory");
    return freq;
}

inline void set_tval(std::uint32_t tval) noexcept {
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 0" : : "r"(tval) : "memory");
}

inline std::uint32_t get_tval() noexcept {
    std::uint32_t tval;
    __asm__ volatile("mrc p15, 0, %0, c14, c2, 0" : "=r"(tval) : : "memory");
    return tval;
}

inline void set_ctl(std::uint32_t ctl) noexcept {
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 1" : : "r"(ctl) : "memory");
}

inline std::uint32_t get_ctl() noexcept {
    std::uint32_t ctl;
    __asm__ volatile("mrc p15, 0, %0, c14, c2, 1" : "=r"(ctl) : : "memory");
    return ctl;
}

/// The physical timer's 64-bit compare value (CNTP_CVAL): the interrupt is
/// due when the counter reaches it.
inline void set_cval(std::uint64_t cval) noexcept {
    const std::uint32_t lo = static_cast<std::uint32_t>(cval);
    const std::uint32_t hi = static_cast<std::uint32_t>(cval >> 32);
    __asm__ volatile("mcrr p15, 2, %0, %1, c14" : : "r"(lo), "r"(hi) : "memory");
}

inline std::uint64_t get_cval() noexcept {
    std::uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 2, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

/// Read the physical generic-timer counter (CNTPCT, 64-bit).
/// Named get_count() to match the AArch64 port: the shared matrix tests
/// call it by name, and it was the only accessor the two ports spelled
/// differently.
inline std::uint64_t get_count() noexcept {
    std::uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

// Resolves the counter's frequency (once: later calls, from the other
// cores, keep the first result, so period_cycles() never changes).
void init() noexcept;

// Starts this core's tick on an absolute grid: compare = start + k * period.
void start_1ms() noexcept;

// In the tick interrupt: the next compare one period after the last one (not
// one period from now, which would add the interrupt's latency to every
// tick and make the kernel's clocks lose time). A tick that comes late is
// caught up: the compare is already due and the interrupt fires again.
void rearm_period() noexcept;

// The counter's value when core 0 started its tick: the origin of the
// kernel's hrclock, so hrclock.now() (count - origin) and the clock's own
// tick-driven count (ticks * cycles_per_tick) agree at every tick.
std::uint64_t hr_base() noexcept;

/// Counter cycles per timer-interrupt period (frequency()/1000/OS_SYSTICK_DIV).
std::uint32_t period_cycles() noexcept;

/// Resolved generic-timer counter frequency (Hz), chosen in init().
/// On real hardware this is 19.2 MHz; under QEMU it mirrors CNTFRQ.
std::uint32_t frequency() noexcept;

} // namespace timer_arm

#endif /* TIMER_ARM_HPP */
