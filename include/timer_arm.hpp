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

/// Read the physical generic-timer counter (CNTPCT, 64-bit).
/// Named get_count() to match the AArch64 port: the shared matrix tests
/// call it by name, and it was the only accessor the two ports spelled
/// differently.
inline std::uint64_t get_count() noexcept {
    std::uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

void init() noexcept;

void start_1ms() noexcept;

/// Counter cycles per timer-interrupt period (frequency()/1000/OS_SYSTICK_DIV).
std::uint32_t period_cycles() noexcept;

/// Resolved generic-timer counter frequency (Hz), chosen in init().
/// On real hardware this is 19.2 MHz; under QEMU it mirrors CNTFRQ.
std::uint32_t frequency() noexcept;

} // namespace timer_arm

#endif /* TIMER_ARM_HPP */
