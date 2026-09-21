/**
 * @file rk3506.hpp
 * @brief RK3506 (Luckfox Lyra B) Hardware Abstraction Layer
 * @details Register access primitives, base addresses, and utility functions
 *          for the RK3506 peripherals used by the StateOS port: UART1 console,
 *          GPIO1 (LED), CRU (core reset release), and the BootROM SRAM mailbox
 *          used to start the secondary Cortex-A7 cores.
 */

#pragma once

#include <cstdint>
#include <type_traits>

namespace rk3506 {

// ============================================================================
// Memory-Mapped Register Access
// ============================================================================

/**
 * @brief Concept for unsigned integral types
 */
template<typename T>
concept UnsignedIntegral = std::is_unsigned_v<T> && std::is_integral_v<T>;

/**
 * @brief Memory-mapped register template
 * @tparam T Register data type
 * @tparam Address Memory-mapped I/O address
 */
template<UnsignedIntegral T, std::uintptr_t Address>
class Register {
public:
    static constexpr std::uintptr_t address = Address;

    [[nodiscard]] static volatile T& ref() noexcept {
        return *reinterpret_cast<volatile T*>(Address);
    }

    [[nodiscard]] static T read() noexcept { return ref(); }

    static void write(T value) noexcept { ref() = value; }

    static void set_bits(T mask) noexcept { ref() |= mask; }

    static void clear_bits(T mask) noexcept { ref() &= ~mask; }

    static void modify(T clear_mask, T set_mask) noexcept {
        ref() = (ref() & ~clear_mask) | set_mask;
    }

    [[nodiscard]] static bool test_bits(T mask) noexcept {
        return (ref() & mask) != 0;
    }
};

/**
 * @brief Rockchip write-mask register template
 * @details Atomic write with mask: upper 16 bits = write enable,
 *          lower 16 bits = data to write. No read-modify-write needed.
 */
template<std::uintptr_t Address>
class WriteMaskRegister {
public:
    static constexpr std::uintptr_t address = Address;

    [[nodiscard]] static volatile std::uint32_t& ref() noexcept {
        return *reinterpret_cast<volatile std::uint32_t*>(Address);
    }

    [[nodiscard]] static std::uint32_t read() noexcept {
        return ref() & 0xFFFF;
    }

    static void write(std::uint16_t mask, std::uint16_t value) noexcept {
        ref() = (static_cast<std::uint32_t>(mask) << 16) | (value & mask);
    }

    static void set_bits(std::uint16_t bits) noexcept { write(bits, bits); }

    static void clear_bits(std::uint16_t bits) noexcept { write(bits, 0); }
};

// ============================================================================
// Base Addresses (RK3506, from boot.md memory map)
// ============================================================================

namespace base {
    inline constexpr std::uintptr_t UART1     = 0xFF0B'0000; // console (set up by miniloader)
    inline constexpr std::uintptr_t GICD      = 0xFF58'1000; // GIC-400 Distributor
    inline constexpr std::uintptr_t GICC      = 0xFF58'2000; // GIC-400 CPU Interface
    inline constexpr std::uintptr_t GPIO1_IOC = 0xFF66'0000; // GPIO1 pinmux
    inline constexpr std::uintptr_t GPIO1     = 0xFF87'0000; // GPIO1 controller (LED on GPIO1_A0)
    inline constexpr std::uintptr_t CRU       = 0xFF9A'0000; // Clock & Reset Unit
    inline constexpr std::uintptr_t SRAM      = 0xFFF8'0000; // System SRAM (BootROM mailbox)
}

// ============================================================================
// UART1 Registers (8250/16550 layout; baud/pinmux already set by bootloader)
// ============================================================================

namespace uart1 {
    using RBR_THR = Register<std::uint32_t, base::UART1 + 0x00>;  // RX/TX buffer
    using IER     = Register<std::uint32_t, base::UART1 + 0x04>;  // Interrupt enable
    using FCR_IIR = Register<std::uint32_t, base::UART1 + 0x08>;  // FIFO control
    using LCR     = Register<std::uint32_t, base::UART1 + 0x0C>;  // Line control
    using MCR     = Register<std::uint32_t, base::UART1 + 0x10>;  // Modem control
    using LSR     = Register<std::uint32_t, base::UART1 + 0x14>;  // Line status

    namespace lsr {
        inline constexpr std::uint32_t THRE = 1 << 5;  // TX holding register empty
        inline constexpr std::uint32_t DR   = 1 << 0;  // Data ready
    }
}

// ============================================================================
// GPIO1 (LED on GPIO1_A0) and its pinmux
// ============================================================================

namespace gpio1 {
    using SWPORT_DR_L  = WriteMaskRegister<base::GPIO1 + 0x0000>; // data, pins A0-B7
    using SWPORT_DDR_L = WriteMaskRegister<base::GPIO1 + 0x0008>; // direction, pins A0-B7
}

namespace gpio1_ioc {
    using GPIO1A_IOMUX_SEL0 = WriteMaskRegister<base::GPIO1_IOC + 0x0020>;
}

// ============================================================================
// CRU (Clock/Reset Unit)
// ============================================================================

namespace cru {
    using CLKGATE_CON03 = WriteMaskRegister<base::CRU + 0x080C>; // GPIO1 ctrl+debounce gates (bits 8,9)
    using CLKGATE_CON22 = WriteMaskRegister<base::CRU + 0x0858>; // GPIO1_IOC gate (bit 1)
    using SOFTRST_CON00 = WriteMaskRegister<base::CRU + 0x0A00>; // CPU core resets
    using SOFTRST_CON03 = WriteMaskRegister<base::CRU + 0x0A0C>; // GPIO1 resets (bits 8,9)
    using SOFTRST_CON22 = WriteMaskRegister<base::CRU + 0x0A58>; // GPIO1_IOC reset (bit 1)

    namespace softrst00 {
        inline constexpr std::uint16_t CORE1_RESETS = (1u << 1) | (1u << 5);
        inline constexpr std::uint16_t CORE2_RESETS = (1u << 2) | (1u << 6);
    }
}

// ============================================================================
// BootROM SRAM mailbox (secondary-core boot handshake)
// ============================================================================

namespace mailbox {
    using MAGIC  = Register<std::uint32_t, base::SRAM + 0x0004>; // 0xDEADBEAF handshake
    using TARGET = Register<std::uint32_t, base::SRAM + 0x0008>; // secondary-core entry PC

    inline constexpr std::uint32_t MAGIC_VALUE = 0xDEADBEAFu;
}

// ============================================================================
// Delay Utilities (ARM Generic Timer, CNTFRQ = 24 MHz set in startup.S)
// ============================================================================

inline std::uint32_t cnt_frq() noexcept {
    std::uint32_t v;
    asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

inline std::uint64_t cnt_pct() noexcept {
    std::uint32_t lo, hi;
    asm volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

inline void udelay(std::uint32_t us) noexcept {
    const std::uint64_t start = cnt_pct();
    const std::uint64_t ticks = (static_cast<std::uint64_t>(us) * cnt_frq()) / 1'000'000u;
    while ((cnt_pct() - start) < ticks)
        asm volatile("dmb" ::: "memory");
}

inline void mdelay(std::uint32_t ms) noexcept { udelay(ms * 1000u); }

// ============================================================================
// Memory Barriers / Events
// ============================================================================

inline void dsb() noexcept { asm volatile("dsb" ::: "memory"); }
inline void isb() noexcept { asm volatile("isb" ::: "memory"); }
inline void dmb() noexcept { asm volatile("dmb" ::: "memory"); }
inline void sev() noexcept { asm volatile("sev" ::: "memory"); }
inline void wfe() noexcept { asm volatile("wfe" ::: "memory"); }

} // namespace rk3506
