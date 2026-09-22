/**
 * @file  uart.hpp
 * @brief Console driver for the Luckfox Lyra B (RK3506, 3x Cortex-A7).
 * @details TX-only, polled, with two backends selected at build time:
 *
 *            - hardware (default): the RK3506 DesignWare 16550 on UART1, the
 *              port wired to the Lyra debug header. The Rockchip miniloader
 *              has already muxed the pins and set the baud rate, so init()
 *              only silences the interrupts and enables the FIFO.
 *            - QEMU_BUILD: the PL011 that `qemu-system-arm -M virt` places at
 *              0x0900_0000. The Lyra's own silicon (GIC layout, CRU, mailbox,
 *              Cortex-M0) is not modelled by QEMU, so the emulated runs use
 *              the plain Cortex-A7 machine and its plain console.
 *
 *          Same streaming API as every other board in this port (`putc`,
 *          `puts`, `putc_uart`, `puts_uart`, `print_hex`, `print_dec`,
 *          `operator<<`, global `uart::uart1`), so the shared test
 *          applications compile unchanged.
 */

#pragma once

#include <cstdint>
#include "rk3506.hpp"

// ---- Board label used by the runtime test banners ---------------------------
// The shared test applications print the board, the ISA and the CPU complex
// through these, so one copy of a test can name the silicon it runs on.
#if defined(QEMU_BUILD)
#define PORT_BANNER_LONG  "QEMU virt (Cortex-A7)"
#define PORT_BANNER_SHORT "QEMU A7"
#define PORT_BANNER_CPU   "3x Cortex-A7 (emulated)"
#else
#define PORT_BANNER_LONG  "Luckfox Lyra B"
#define PORT_BANNER_SHORT "Lyra B"
#define PORT_BANNER_CPU   "RK3506 (3x Cortex-A7 + 1x Cortex-M0)"
#endif

#define PORT_BANNER_ISA   "AArch32"

// SEMIHOST is defined only by the hardware-run build (see hw.sh / CMakeLists).
// When set, every console byte is ALSO echoed through the ARM semihosting
// channel, so OpenOCD ("arm semihosting enable") prints the test console to
// its log even though the board has no USB-serial connected to the host.
#if defined(SEMIHOST)
#include "semihosting.hpp"
#endif

namespace uart {

/**
 * @class Uart1
 * @brief Console driver (TX only, polled).
 */
class Uart1 {
public:
    Uart1() = default;

    Uart1(const Uart1&) = delete;
    Uart1& operator=(const Uart1&) = delete;
    Uart1(Uart1&&) = delete;
    Uart1& operator=(Uart1&&) = delete;

#if defined(QEMU_BUILD)

    /// PL011 as `-M virt` maps it. QEMU ignores the baud divisors, but the
    /// enable bits and the FIFO still have to be programmed.
    void init() noexcept {
        mmio_write(pl011::CR, 0x0);
        mmio_write(pl011::ICR, 0x7FF);
        mmio_write(pl011::IBRD, 26u);                       // 115200 @ 48 MHz
        mmio_write(pl011::FBRD, 3u);
        mmio_write(pl011::LCRH, (3u << 5) | (1u << 4));     // 8N1, FIFO on
        mmio_write(pl011::IMSC, 0x0);                       // polled
        mmio_write(pl011::CR, (1u << 0) | (1u << 8) | (1u << 9));
        initialized_ = true;
    }

#else

    /**
     * @brief Initialize UART1.
     * @details Pinmux, clocks and baud rate are inherited from the bootloader;
     *          just make sure interrupts are off and the FIFO is enabled.
     */
    void init() noexcept {
        using namespace rk3506;
        uart1::IER::write(0x00);      // no UART interrupts (polled TX)
        uart1::FCR_IIR::write(0x01);  // FIFO enable
#if defined(UART_BAUD)
        // Opt-in baud reprogramming (build with -DUART_BAUD=1500000).
        // sclk_uart1 = 24 MHz (bootloader); 16550 divisor = clk/(16*baud):
        // 115200 -> 13 (bootloader default), 1500000 -> 1 (exact).
        // Wait for the transmitter to fully drain first — the DW UART
        // raises "busy detect" if LCR is written mid-character.
        while (!uart1::LSR::test_bits(1u << 6)) {  // LSR.TEMT
        }
        constexpr std::uint32_t div = 24000000u / (16u * (UART_BAUD));
        static_assert(div >= 1, "UART_BAUD too high for 24 MHz sclk");
        uart1::LCR::set_bits(0x80);                // DLAB=1
        uart1::RBR_THR::write(div & 0xFFu);        // DLL
        uart1::IER::write((div >> 8) & 0xFFu);     // DLH
        uart1::LCR::clear_bits(0x80);              // DLAB=0
        uart1::IER::write(0x00);                   // interrupts still off
#endif
        initialized_ = true;
    }

#endif // QEMU_BUILD

    /**
     * @brief Transmit a single character (blocking), mirrored to semihosting.
     * @param c Character to send.
     */
    void putc(char c) const noexcept {
        write_raw(c);
        sh_putc(c);
    }

    /**
     * @brief Transmit a null-terminated string ('\n' -> CR+LF).
     * @param str Null-terminated string to send.
     */
    void puts(const char* str) const noexcept {
        for (const char* p = str; *p; ++p) {
            if (*p == '\n')
                write_raw('\r');
            write_raw(*p);
            // Buffered: the semihosting mirror emits one SYS_WRITE0 per line,
            // not one SYS_WRITEC per byte. See sh_putc().
            sh_putc(*p);
        }
    }

    /// Emit any buffered semihosting text now.
    ///
    /// The mirror batches whole lines, so a caller that prints a partial line
    /// and then blocks or exits should flush. Most output ends in '\n' and
    /// flushes itself.
    void flush() const noexcept { sh_flush(); }

    // Console-only output: goes to the UART but is NOT echoed to semihosting.
    // Used when a thread must print to the serial console without paying the
    // semihosting-trap cost (and without spamming the OpenOCD log).

    /// @param c Character to send to the UART alone.
    void putc_uart(char c) const noexcept {
        write_raw(c);
    }

    /// @param str Null-terminated string to send to the UART alone.
    void puts_uart(const char* str) const noexcept {
        for (const char* p = str; *p; ++p) {
            if (*p == '\n')
                write_raw('\r');
            write_raw(*p);
        }
    }

    /**
     * @brief Print unsigned integer in decimal.
     * @param value Value to print.
     */
    void print_dec(std::uint32_t value) const noexcept {
        char digits[10];
        int pos = 0;
        if (value == 0) {
            digits[pos++] = '0';
        } else {
            while (value > 0) {
                digits[pos++] = static_cast<char>('0' + (value % 10));
                value /= 10;
            }
        }
        char out[11];
        int n = 0;
        while (pos > 0)
            out[n++] = digits[--pos];
        out[n] = '\0';
        puts(out);
    }

    /**
     * @brief Print unsigned integer in hexadecimal.
     * @param value Value to print.
     * @param width Number of hex digits (default 8).
     */
    void print_hex(std::uint32_t value, int width = 8) const noexcept {
        static constexpr char hex_chars[] = "0123456789ABCDEF";
        if (width < 1)
            width = 1;
        if (width > 8)
            width = 8;
        char out[9];
        int n = 0;
        for (int i = width - 1; i >= 0; --i)
            out[n++] = hex_chars[(value >> (i * 4)) & 0xF];
        out[n] = '\0';
        puts(out);
    }

    /// @return true once init() has run.
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // Stream-like operators

    /// Stream a C string. @param str Null-terminated text. @return *this for chaining.
    const Uart1& operator<<(const char* str) const noexcept {
        puts(str);
        return *this;
    }

    /// Stream a single character. @param c Character. @return *this for chaining.
    const Uart1& operator<<(char c) const noexcept {
        putc(c);
        return *this;
    }

    /// Stream an unsigned integer in decimal. @param value Value. @return *this for chaining.
    const Uart1& operator<<(std::uint32_t value) const noexcept {
        print_dec(value);
        return *this;
    }

    /// Stream a signed integer in decimal. @param value Value. @return *this for chaining.
    const Uart1& operator<<(int value) const noexcept {
        // Negate in unsigned space so INT_MIN does not overflow.
        std::uint32_t mag = (value < 0)
            ? (0u - static_cast<std::uint32_t>(value))
            : static_cast<std::uint32_t>(value);
        if (value < 0)
            putc('-');
        print_dec(mag);
        return *this;
    }

private:
#if defined(QEMU_BUILD)

    // PL011 as instantiated by `qemu-system-arm -M virt`.
    struct pl011 {
        static constexpr std::uintptr_t BASE = 0x0900'0000;
        static constexpr std::uintptr_t DR   = BASE + 0x00;
        static constexpr std::uintptr_t FR   = BASE + 0x18;
        static constexpr std::uintptr_t IBRD = BASE + 0x24;
        static constexpr std::uintptr_t FBRD = BASE + 0x28;
        static constexpr std::uintptr_t LCRH = BASE + 0x2C;
        static constexpr std::uintptr_t CR   = BASE + 0x30;
        static constexpr std::uintptr_t IMSC = BASE + 0x38;
        static constexpr std::uintptr_t ICR  = BASE + 0x44;
    };

    static void mmio_write(std::uintptr_t addr, std::uint32_t value) noexcept {
        *reinterpret_cast<volatile std::uint32_t*>(addr) = value;
    }

    static std::uint32_t mmio_read(std::uintptr_t addr) noexcept {
        return *reinterpret_cast<volatile std::uint32_t*>(addr);
    }

    void write_raw(char c) const noexcept {
        while (mmio_read(pl011::FR) & (1u << 5)) {  // FR.TXFF
        }
        mmio_write(pl011::DR,
                   static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
    }

#else

    void write_raw(char c) const noexcept {
        using namespace rk3506;
        while (!uart1::LSR::test_bits(uart1::lsr::THRE)) {
            // spin until TX holding register empty
        }
        uart1::RBR_THR::write(static_cast<std::uint32_t>(
            static_cast<unsigned char>(c)));
    }

#endif // QEMU_BUILD

    // --- Semihosting mirror, line buffered ---------------------------------
    //
    // Every semihosting call is a debug halt/resume over SWD, and the cost is
    // per CALL, not per byte: a SYS_WRITEC per character is what made the
    // console crawl (a few characters a second). So the mirror accumulates a
    // line and emits it with a single SYS_WRITE0 at the newline (or when the
    // buffer is full).
    //
    // The buffer is per-instance and is touched only through putc()/puts(),
    // which the multi-core tests serialise with their console mutex.
    static constexpr unsigned sh_buf_size_ = 256;
    mutable char sh_buf_[sh_buf_size_];
    mutable unsigned sh_len_ = 0;

    void sh_flush() const noexcept {
#if defined(SEMIHOST)
        if (sh_len_ > 0) {
            sh_buf_[sh_len_] = '\0';
            semihosting::write_str(sh_buf_);
            sh_len_ = 0;
        }
#endif
    }

    void sh_putc(char c) const noexcept {
#if defined(SEMIHOST)
        if (sh_len_ + 1u >= sh_buf_size_) {
            sh_flush();
        }
        sh_buf_[sh_len_++] = c;
        if (c == '\n' || c == '\r') {
            sh_flush();
        }
#else
        (void)c;
#endif
    }

    bool initialized_ = false;   ///< Set by init().
};

/**
 * @brief Global console instance.
 */
inline Uart1 uart1;

/**
 * @brief Newline constant for streaming output.
 */
inline constexpr const char* endl = "\n";

} // namespace uart
