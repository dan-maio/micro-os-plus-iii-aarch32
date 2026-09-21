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

void start_1ms() noexcept {
    set_tval(period_cycles());
    set_ctl(0x1);  // ENABLE=1, IMASK=0
}

} // namespace timer_arm
