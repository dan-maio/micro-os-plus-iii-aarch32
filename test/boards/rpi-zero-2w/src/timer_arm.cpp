/**
 * @file timer_arm.cpp
 * @brief ARM Generic Timer implementation
 * @details Implementation of ARM Generic Timer initialization and configuration.
 */

#include <timer_arm.hpp>

// BCM2837 system-timer (free-running, fixed 1 MHz) used to calibrate the
// ARM generic-timer counter. MMU already maps the peripheral block.
#define BCM2837_ST_BASE 0x3F003000u
#define BCM2837_ST_CLO  0x04u

namespace timer_arm {

namespace {
// Resolved counter frequency, chosen once in init(). Do NOT trust/write
// CNTFRQ: it only *reports* a rate, writing it from EL1/SVC is UNDEFINED,
// and on this board it lies (19.2 MHz reported vs ~1 MHz actual).
volatile std::uint32_t g_freq = 19'200'000u;

// Read the BCM2835 system-timer free-running counter (1 MHz).
std::uint32_t st_clo() noexcept {
    return *reinterpret_cast<volatile std::uint32_t*>(BCM2837_ST_BASE + BCM2837_ST_CLO);
}

// Measure the ARM generic-timer counter rate against the 1 MHz system timer
// over a ~50 ms window.  Returns the measured frequency in Hz.
std::uint32_t calibrate_hw_freq() noexcept {
    // Wait for a clean 1 MHz edge so both windows start aligned.
    std::uint32_t a0 = st_clo();
    while (st_clo() == a0) { }
    std::uint64_t t0 = get_count();
    std::uint32_t s0 = st_clo();

    while ((st_clo() - s0) < 50000u) { }   // 50 ms @ 1 MHz

    std::uint64_t t1 = get_count();
    std::uint32_t s1 = st_clo();
    std::uint32_t dur = s1 - s0;           // us
    if (dur == 0u) { dur = 50000u; }
    return static_cast<std::uint32_t>((t1 - t0) * 1000000ull / dur);
}
}

std::uint32_t frequency() noexcept { return g_freq; }

std::uint32_t period_cycles() noexcept { return g_freq / 1000u / OS_SYSTICK_DIV; }

void init() noexcept {
    // Do NOT trust CNTFRQ: on this board it reports 19.2 MHz while the
    // physical generic-timer counter actually increments at ~1 MHz. Always
    // measure the real rate against the fixed 1 MHz system timer.
    // Once: port_sys_init() runs on every core, and a recalibration by each
    // would change period_cycles() while the cores start.
    static bool calibrated = false;
    if (calibrated)
      {
        return;
      }
    std::uint32_t f = calibrate_hw_freq();
    if (f == 0u)
      {
        f = 19'200'000u;
      }
    g_freq = f;
    calibrated = true;
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
