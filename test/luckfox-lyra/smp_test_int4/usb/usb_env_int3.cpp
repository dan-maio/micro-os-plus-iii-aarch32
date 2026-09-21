/**
 * @file usb_env_int3.cpp
 * @brief Minimal environment shim for usb1_int3.cpp (smp_test_int3).
 *
 * Only the services the int3 OTG1 driver actually uses:
 *   time   udelay/mdelay -> CNTPCT generic counter (24 MHz, set by startup.S)
 *   cache  flush/invalidate_dcache_range -> no-ops: every DMA buffer lives
 *          in the Normal-NC .dma_nc window
 *
 * Console output does NOT go through uart_puts here: the driver calls
 * app_console_write() (main.cpp), which routes into the IRQ-driven TX ring.
 */

#include <cstdint>

namespace {

inline std::uint64_t cntpct() {
    std::uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}
constexpr std::uint32_t kCntHz = 24000000u;

} // namespace

extern "C" {

/// Busy-wait microseconds using CNTPCT. @param us Microseconds to spin.
__attribute__((weak)) void udelay(unsigned long us) {
    const std::uint64_t end = cntpct() + (static_cast<std::uint64_t>(us) * kCntHz) / 1000000u;
    while (cntpct() < end) { }
}
/// Busy-wait milliseconds. @param ms Milliseconds to spin.
__attribute__((weak)) void mdelay(unsigned long ms) { udelay(ms * 1000u); }

/// D-cache invalidate hook — no-op: all DMA buffers live in .dma_nc.
__attribute__((weak)) void invalidate_dcache_range(std::uint32_t, std::uint32_t) { }
/// D-cache clean hook — no-op: all DMA buffers live in .dma_nc.
__attribute__((weak)) void flush_dcache_range(std::uint32_t, std::uint32_t) { }

} /* extern "C" */
