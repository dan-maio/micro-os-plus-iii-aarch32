/**
 * @file  uart.hpp
 * @brief PL011 UART0 console for the Raspberry Pi Zero 2W (BCM2837).
 * @details TX-only, polled. Same streaming API (`putc`, `puts`, `print_hex`,
 *          `print_dec`, `operator<<`, global `uart::uart1`) as the RK3506 a7
 *          port so all kernel/demo code compiles unchanged. The Pi firmware
 *          (enable_uart=1) already muxes GPIO14/15 to UART0 and clocks it, so
 *          init() only (re)programs 115200 8N1 and enables TX.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "bcm2837.hpp"

// ---- Board label used by the runtime test banners ---------------------------
// The same sources build for the Pi Zero 2 W (default) or the Pi 3 B
// (BOARD_RPI3B / make BOARD=rpi3b). These expand to the current board's name
// so the printed banners match the silicon actually running.
#ifdef BOARD_RPI3B
#define PORT_BANNER_LONG  "Raspberry Pi 3 B"
#define PORT_BANNER_SHORT "RPi 3 B"
#else
#define PORT_BANNER_LONG  "Raspberry Pi Zero 2W"
#define PORT_BANNER_SHORT "RPi Zero 2W"
#endif

// The shared test applications print the ISA and the CPU complex through
// these, so one copy of a test can name the silicon it is running on.
#define PORT_BANNER_ISA   "AArch32"
#define PORT_BANNER_CPU   "BCM2837 (4x Cortex-A53, AArch32)"


// SEMIHOST is defined by the qemu.sh / hw.sh builds. When set, console output
// is ALSO emitted through the ARM semihosting channel, so the emulator or
// OpenOCD ("arm semihosting enable") receives it even though the board has no
// USB-serial connected to the host.
#if defined(SEMIHOST)
#include "semihosting.hpp"
#endif

// ---- What goes through semihosting ------------------------------------------
// The same policy as the Raspberry Pi 4 B and the AArch64 port. Every
// semihosting call halts the core until OpenOCD has served it over JTAG,
// which costs from tens of milliseconds to seconds per call; with every
// console line mirrored, a hardware run printed at the speed of the probe and
// not of the UART. So:
//   - under QEMU (no HW_BUILD) everything is mirrored: semihosting is the
//     console there, and it is fast;
//   - on the board only the verdict lines (RESULT / FAIL / FATAL) are
//     mirrored, which is all the runner reads; the full console is on the
//     UART (GPIO14/15, 115200 8N1);
//   - SEMI_PRINTS=1 at configure time also mirrors what core 0 prints, for a
//     run without a terminal on the UART.
// 1: mirror every console byte to semihosting; 0: only the verdict lines.
#if defined(SEMIHOST) && !defined(HW_BUILD)
#define PORT_SEMIHOST_MIRROR 1
#else
#define PORT_SEMIHOST_MIRROR 0
#endif

// 1: on the board, also mirror core 0's console lines (SEMI_PRINTS=1).
#if defined(SEMIHOST) && defined(HW_BUILD) && defined(SEMI_PRINTS) && SEMI_PRINTS
#define PORT_SEMI_PRINTS_CORE0 1
#else
#define PORT_SEMI_PRINTS_CORE0 0
#endif

namespace uart {

namespace detail {
// True on core 0 (MPIDR.Aff0; the BCM2837 numbers its cores 0-3 there).
inline bool on_core0() noexcept {
    std::uint32_t mpidr;
    asm volatile("mrc p15, 0, %0, c13, c0, 0" : "=r"(mpidr));
    return (mpidr & 0xFFu) == 0;
}

// True for a line the hardware runner must see: the verdict or a failure.
inline bool is_verdict(const char* s) noexcept {
    static constexpr const char* keys[] = { "RESULT", "FAIL", "FATAL" };
    for (const char* k : keys) {
        for (const char* p = s; *p; ++p) {
            const char* a = p;
            const char* b = k;
            while (*a && *b && *a == *b) { ++a; ++b; }
            if (*b == '\0') return true;
        }
    }
    return false;
}

// is_verdict() for a buffer that is not NUL-terminated (newlib's _write).
inline bool is_verdict_n(const char* s, std::size_t n) noexcept {
    static constexpr const char* keys[] = { "RESULT", "FAIL", "FATAL" };
    for (const char* k : keys) {
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t j = 0;
            while (k[j] && i + j < n && s[i + j] == k[j]) ++j;
            if (k[j] == '\0') return true;
        }
    }
    return false;
}

// Whether a console piece goes to semihosting on a board that mirrors only
// the verdict. A verdict is often printed in pieces ("\nRESULT: ", "PASS",
// "\n"), so once a piece holding a key is mirrored, the following pieces are
// mirrored too, up to the end of that line.
inline bool& verdict_line_open() noexcept {
    static bool open = false;
    return open;
}

inline bool mirror_verdict(const char* s, std::size_t n) noexcept {
    const bool mirror = verdict_line_open() || is_verdict_n(s, n);
    if (mirror && n != 0)
        verdict_line_open() = (s[n - 1] != '\n');
    return mirror;
}
} // namespace detail

class Uart1 {
public:
    Uart1() = default;
    Uart1(const Uart1&) = delete;
    Uart1& operator=(const Uart1&) = delete;

    void init() noexcept {
        using namespace bcm;
        // Disable UART while reconfiguring.
        mmio_write(pl011::CR, 0x0);
        // Clear pending interrupts.
        mmio_write(pl011::ICR, 0x7FF);
        // 115200 baud @ 48 MHz UART clock: IBRD=26, FBRD=3.
        mmio_write(pl011::IBRD, 26u);
        mmio_write(pl011::FBRD, 3u);
        // 8N1, FIFOs enabled (WLEN=11b<<5, FEN<<4).
        mmio_write(pl011::LCRH, (3u << 5) | (1u << 4));
        // Mask all UART interrupts (polled).
        mmio_write(pl011::IMSC, 0x0);
        // Enable UART + TX (+ RX).
        mmio_write(pl011::CR, (1u << 0) | (1u << 8) | (1u << 9));
        initialized_ = true;
    }

    void putc(char c) const noexcept {
        write_raw(c);
#if PORT_SEMIHOST_MIRROR
        semihosting::write_char(c);
#elif PORT_SEMI_PRINTS_CORE0
        if (detail::on_core0())
            semihosting::write_char(c);
#endif
    }

    void puts(const char* str) const noexcept {
        for (const char* p = str; *p; ++p) {
            if (*p == '\n')
                write_raw('\r');
            write_raw(*p);
        }
#if PORT_SEMIHOST_MIRROR
        // Whole string in ONE SYS_WRITE0 — far cheaper than a trap per byte.
        semihosting::write_str(str);
#elif defined(SEMIHOST)
        std::size_t n = 0;
        while (str[n]) ++n;
        if (detail::mirror_verdict(str, n))
            semihosting::write_str(str);
#if PORT_SEMI_PRINTS_CORE0
        else if (detail::on_core0())
            semihosting::write_str(str);
#endif
#endif
    }

    // UART-only output: goes to the PL011 but is NOT echoed to semihosting.
    // Used when a thread must print to the serial console without paying the
    // semihosting-trap cost (and without spamming the OpenOCD log).
    void putc_uart(char c) const noexcept {
        write_raw(c);
    }

    void puts_uart(const char* str) const noexcept {
        for (const char* p = str; *p; ++p) {
            if (*p == '\n')
                write_raw('\r');
            write_raw(*p);
        }
    }

private:
    void write_raw(char c) const noexcept {
        using namespace bcm;
        while (mmio_read(pl011::FR) & pl011::FR_TXFF) { /* TX FIFO full */ }
        mmio_write(pl011::DR, static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
    }

public:

    // Numbers are formatted into a local buffer and emitted with a SINGLE
    // puts() (one SYS_WRITE0), never one putc()/SYS_WRITEC per digit. On a
    // J-Link/OpenOCD run every semihosting trap is a debug halt plus a JTAG
    // round-trip, and while a core is halted it takes no timer interrupts, so
    // per-digit traps both starve that core and lose kernel ticks. A
    // smp_test4 reporter pass costs ~29 traps this way instead of ~240.
    void print_dec(std::uint32_t value) const noexcept {
        char digits[10];
        int pos = 0;
        if (value == 0) { digits[pos++] = '0'; }
        else { while (value > 0) { digits[pos++] = static_cast<char>('0' + (value % 10)); value /= 10; } }
        char out[11];
        int n = 0;
        while (pos > 0) out[n++] = digits[--pos];
        out[n] = '\0';
        puts(out);
    }

    void print_hex(std::uint32_t value, int width = 8) const noexcept {
        static constexpr char hex_chars[] = "0123456789ABCDEF";
        if (width < 1) width = 1;
        if (width > 8) width = 8;
        char out[9];
        int n = 0;
        for (int i = width - 1; i >= 0; --i)
            out[n++] = hex_chars[(value >> (i * 4)) & 0xF];
        out[n] = '\0';
        puts(out);
    }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    const Uart1& operator<<(const char* str) const noexcept { puts(str); return *this; }
    const Uart1& operator<<(char c) const noexcept { putc(c); return *this; }
    const Uart1& operator<<(std::uint32_t value) const noexcept { print_dec(value); return *this; }
    const Uart1& operator<<(int value) const noexcept {
        // Negate in unsigned to keep INT_MIN well defined.
        std::uint32_t mag = (value < 0)
            ? (0u - static_cast<std::uint32_t>(value))
            : static_cast<std::uint32_t>(value);
        if (value < 0) putc('-');
        print_dec(mag);
        return *this;
    }

private:
    bool initialized_ = false;
};

inline Uart1 uart1;
inline constexpr const char* endl = "\n";

} // namespace uart
