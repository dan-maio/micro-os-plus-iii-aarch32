/*
 * timer_arm.cpp - ARM generic-timer set-up for the Luckfox Lyra B (RK3506).
 *
 * The accessors are shared (include/timer_arm.hpp); only the counter's rate
 * is a board fact. On the RK3506 CNTFRQ is programmed by the miniloader and
 * is honest, so init() reads it and only falls back to the 24 MHz OSC rate
 * if it comes back zero -- which is what happens under QEMU's `virt` when
 * nothing has written it.
 */

#include <timer_arm.hpp>

namespace timer_arm {

namespace {
// Resolved counter frequency, chosen once in init().
volatile std::uint32_t g_freq = 24'000'000u;
}

std::uint32_t frequency() noexcept { return g_freq; }

std::uint32_t period_cycles() noexcept { return g_freq / 1000u / OS_SYSTICK_DIV; }

void init() noexcept {
    std::uint32_t freq = get_freq();
    if (freq == 0u) {
        freq = 24'000'000u;
        set_freq(freq);
    }
    g_freq = freq;
}

namespace {
volatile std::uint64_t g_base = 0;   // core 0's tick origin
volatile bool g_base_set = false;
}

void start_1ms() noexcept {
    const std::uint64_t now = get_count();
    if (!g_base_set) {  // core 0, first: the hrclock origin
        g_base = now;
        g_base_set = true;
    }
    set_cval(now + period_cycles());
    set_ctl(0x1);  // ENABLE=1, IMASK=0
}

void rearm_period() noexcept {
    const std::uint32_t period = period_cycles();
    std::uint64_t next = get_cval() + period;
    // Far behind (a core halted by the debugger): resynchronise rather than
    // replay thousands of ticks.
    const std::uint64_t now = get_count();
    if (now > next + 1000ull * period) {
        next = now + period;
    }
    set_cval(next);
}

std::uint64_t hr_base() noexcept {
    return g_base;
}

} // namespace timer_arm
