/**
 * @file usb_env_stateos.cpp
 * @brief Environment shim: the U-Boot-style USB stack under StateOS/newlib.
 *
 * Replaces the standalone firmware's stub.cpp + board_init.cpp services:
 *
 *   console   uart_putc/uart_puts/printf  -> UART1 driver (raw, unmutexed:
 *             driver debug output may come from deep call chains)
 *   time      udelay/mdelay/get_timer     -> CNTPCT generic counter (24 MHz)
 *   cache     flush/invalidate_dcache_range -> no-ops: every DMA buffer
 *             lives in the Normal-NC .dma_nc window (see dma_pool.hpp)
 *   memory    memalign -> dma_pool (U-Boot gadget code allocates ALL its
 *             DMA buffers via memalign, so this one hook moves them all
 *             into the NC window with zero USB-source edits);
 *             malloc/calloc stay newlib; free() dispatches by address
 *
 * String/memory/snprintf functions come from newlib. All C linkage.
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <reent.h>

#include "uart.hpp"
#include "dma_pool.hpp"

namespace {

inline std::uint64_t cntpct() {
    std::uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}
constexpr std::uint32_t kCntHz = 24000000u;   // CNTFRQ set by startup.S

} // namespace

extern "C" {

/* ------------------------------- console -------------------------------- */
/// Write one character to the console. @param c Character.
void uart_putc(char c) { uart::uart1.putc(c == '\n' ? '\n' : c); }
/// Write a string to the console. @param s Null-terminated text.
void uart_puts(const char* s) { uart::uart1.puts(s); }

/* Rotating hex formatter (up to 4 live values per printf, as in the old
 * board_init.cpp). */
/// Rotating hex formatter. @param val Value. @return "0xXXXXXXXX" in one of four rotated static buffers.
char* hex32(std::uint32_t val) {
    static char bufs[4][11];
    static unsigned idx = 0;
    char* buf = bufs[idx++ & 3u];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = "0123456789ABCDEF"[val & 0xFu];
        val >>= 4;
    }
    buf[10] = '\0';
    return buf;
}

/// Minimal printf routed to UART1. @param fmt printf format string. @return characters formatted, or <0.
int printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        uart::uart1.puts(buf);
    return n;
}

/* -------------------------------- time ---------------------------------- */
/// Busy-wait microseconds using CNTPCT. @param us Microseconds to spin.
void udelay(unsigned long us) {
    const std::uint64_t end = cntpct() + (static_cast<std::uint64_t>(us) * kCntHz) / 1000000u;
    while (cntpct() < end) { }
}
/// Busy-wait milliseconds. @param ms Milliseconds to spin.
void mdelay(unsigned long ms) { udelay(ms * 1000u); }

/// U-Boot get_timer: milliseconds elapsed since @p base. @param base Reference timestamp (ms). @return ms since @p base.
unsigned long get_timer(unsigned long base) {
    return static_cast<unsigned long>(cntpct() / (kCntHz / 1000u)) - base;
}
void increment_timer() { }   ///< U-Boot tick hook (unused; the counter is free-running).
void init_gd() { }           ///< U-Boot global-data init hook (unused here).

/* -------------------------------- cache --------------------------------- */
/// D-cache invalidate hook — invalidates lines to support arbitrary caller buffers.
void invalidate_dcache_range(std::uint32_t start, std::uint32_t stop) {
    std::uint32_t a = start & ~63u;
    for (; a < stop; a += 64u) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory"); // DCIMVAC
    }
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

/// D-cache clean hook — cleans lines to PoC before device DMA reads.
void flush_dcache_range(std::uint32_t start, std::uint32_t stop) {
    std::uint32_t a = start & ~63u;
    for (; a < stop; a += 64u) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory"); // DCCIMVAC
    }
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

/* -------------------------------- memory -------------------------------- */
/// Diagnostic flag printed by usb_composite.cpp (was stub.cpp's heap ptr).
std::uint32_t malloc_ptr = 1;

/// Aligned allocation routed to the DMA pool. @param alignment Alignment (0 => 64). @param size Bytes. @return NC pointer or nullptr.
void* memalign(unsigned long alignment, unsigned long size) {
    return dma_pool::alloc(size, alignment ? alignment : 64);
}

/**
 * @brief free() pre-hook: reclaim DMA-pool pointers before the newlib heap sees them.
 * @param p Pointer being freed.
 * @return true if @p p belonged to the DMA pool (and was freed here); false to fall through to newlib.
 */
bool usb_dma_try_free(void* p) {
    if (!dma_pool::owns(p))
        return false;
    dma_pool::free_(p);
    return true;
}

/// strtoul shim for the U-Boot sources. @param s String. @param ep End pointer out. @param base Radix. @return parsed value.
unsigned long simple_strtoul(const char* s, char** ep, unsigned int base) {
    return strtoul(s, ep, static_cast<int>(base));
}

} /* extern "C" */
