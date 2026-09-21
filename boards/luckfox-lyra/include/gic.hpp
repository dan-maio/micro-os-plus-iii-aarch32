/**
 * @file gic.hpp
 * @brief GIC-400 Interrupt Controller Driver for RK3506 (Luckfox Lyra)
 * @details Provides GIC (Generic Interrupt Controller) initialization and control
 *          for the ARM Cortex-A7 processor on RK3506 (Luckfox Lyra).
 */

#ifndef GIC_HPP
#define GIC_HPP

#include <cstdint>
#include "rk3506.hpp"

// Import Register template from rk3506 namespace
using rk3506::Register;

namespace gic {

// =============================================================================
// GIC-400 Register Definitions for RK3506 (Luckfox Lyra) (ARM Cortex-A7)
// =============================================================================

/**
 * @brief GIC Distributor base address
 */
inline constexpr std::uintptr_t GICD_BASE = 0xFF581000;

/**
 * @brief GIC CPU Interface base address
 */
inline constexpr std::uintptr_t GICC_BASE = 0xFF582000;

// =============================================================================
// GIC Distributor Registers (GICD)
// =============================================================================

namespace gicd {
    // Distributor Control Register
    using CTLR = Register<std::uint32_t, GICD_BASE + 0x000>;

    // Interrupt Controller Type Register
    using TYPER = Register<std::uint32_t, GICD_BASE + 0x004>;

    // Distributor Implementer Identification Register
    using IIDR = Register<std::uint32_t, GICD_BASE + 0x008>;

    /**
     * @brief Interrupt Set-Enable Register base
     */
    inline constexpr std::uintptr_t ISENABLER_BASE = GICD_BASE + 0x100;

    /**
     * @brief Interrupt Clear-Enable Register base
     */
    inline constexpr std::uintptr_t ICENABLER_BASE = GICD_BASE + 0x180;

    /**
     * @brief Interrupt Set-Pending Register base
     */
    inline constexpr std::uintptr_t ISPENDR_BASE = GICD_BASE + 0x200;

    /**
     * @brief Interrupt Clear-Pending Register base
     */
    inline constexpr std::uintptr_t ICPENDR_BASE = GICD_BASE + 0x280;

    /**
     * @brief Interrupt Set-Active Register base
     */
    inline constexpr std::uintptr_t ISACTIVER_BASE = GICD_BASE + 0x300;

    /**
     * @brief Interrupt Clear-Active Register base
     */
    inline constexpr std::uintptr_t ICACTIVER_BASE = GICD_BASE + 0x380;

    /**
     * @brief Interrupt Priority Register base
     */
    inline constexpr std::uintptr_t IPRIORITYR_BASE = GICD_BASE + 0x400;

    /**
     * @brief Interrupt Processor Targets Register base
     */
    inline constexpr std::uintptr_t ITARGETSR_BASE = GICD_BASE + 0x800;

    /**
     * @brief Interrupt Configuration Register base
     */
    inline constexpr std::uintptr_t ICFGR_BASE = GICD_BASE + 0xC00;

    // Software Generated Interrupt Register
    using SGIR = Register<std::uint32_t, GICD_BASE + 0xF00>;

    namespace ctrl {
        inline constexpr std::uint32_t ENABLE_GRP0 = (1U << 0);
        inline constexpr std::uint32_t ENABLE_GRP1 = (1U << 1);
    }
}

// =============================================================================
// GIC CPU Interface Registers (GICC)
// =============================================================================

namespace gicc {
    // CPU Interface Control Register
    using CTLR = Register<std::uint32_t, GICC_BASE + 0x000>;

    // Interrupt Priority Mask Register
    using PMR = Register<std::uint32_t, GICC_BASE + 0x004>;

    // Binary Point Register
    using BPR = Register<std::uint32_t, GICC_BASE + 0x008>;

    // Interrupt Acknowledge Register
    using IAR = Register<std::uint32_t, GICC_BASE + 0x00C>;

    // End of Interrupt Register
    using EOIR = Register<std::uint32_t, GICC_BASE + 0x010>;

    // Running Priority Register
    using RPR = Register<std::uint32_t, GICC_BASE + 0x014>;

    // Highest Priority Pending Interrupt Register
    using HPPIR = Register<std::uint32_t, GICC_BASE + 0x018>;

    namespace ctrl {
        inline constexpr std::uint32_t ENABLE_GRP0 = (1U << 0);
        inline constexpr std::uint32_t ENABLE_GRP1 = (1U << 1);
        inline constexpr std::uint32_t FIQ_EN      = (1U << 3);
    }
}

// =============================================================================
// Interrupt IDs
// =============================================================================

namespace irq {
    inline constexpr std::uint32_t TIMER0 = 32 + 70;  // 102
    inline constexpr std::uint32_t PHYSICAL_TIMER = 29;
    inline constexpr std::uint32_t PHYSICAL_TIMER_NS = 30;
    inline constexpr std::uint32_t SPURIOUS = 1023;
    inline constexpr std::uint32_t UART1  = 67;  // SPI 35 → GIC ID 67 (board-verified via GICD_ISPENDR)
}

// =============================================================================
// GIC Controller Class
// =============================================================================

class Gic {
public:
    void init() noexcept {
        gicd::CTLR::write(0);

        std::uint32_t typer = gicd::TYPER::read();
        num_irqs_ = ((typer & 0x1F) + 1) * 32;

        for (std::uint32_t i = 0; i < num_irqs_ / 32; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::ICENABLER_BASE + i * 4);
            *reg = 0xFFFFFFFF;
        }

        for (std::uint32_t i = 0; i < num_irqs_ / 32; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::ICPENDR_BASE + i * 4);
            *reg = 0xFFFFFFFF;
        }

        for (std::uint32_t i = 0; i < num_irqs_ / 4; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::IPRIORITYR_BASE + i * 4);
            *reg = 0xA0A0A0A0;
        }

        for (std::uint32_t i = 8; i < num_irqs_ / 4; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::ITARGETSR_BASE + i * 4);
            *reg = 0x01010101;
        }

        for (std::uint32_t i = 2; i < num_irqs_ / 16; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::ICFGR_BASE + i * 4);
            *reg = 0x00000000;
        }

        gicd::CTLR::write(gicd::ctrl::ENABLE_GRP0 | gicd::ctrl::ENABLE_GRP1);

        init_cpu_interface();
    }

    void init_cpu_interface() noexcept {
        for (std::uint32_t i = 0; i < 8; ++i) {
            volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
                gicd::IPRIORITYR_BASE + i * 4);
            *reg = 0xA0A0A0A0;
        }

        gicc::PMR::write(0xFF);
        gicc::BPR::write(0);
        gicc::CTLR::write(gicc::ctrl::ENABLE_GRP0 | gicc::ctrl::ENABLE_GRP1);
    }

    void enable_irq(std::uint32_t irq_id) noexcept {
        std::uint32_t reg_index = irq_id / 32;
        std::uint32_t bit_offset = irq_id % 32;

        volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
            gicd::ISENABLER_BASE + reg_index * 4);
        *reg = (1U << bit_offset);
    }

    void disable_irq(std::uint32_t irq_id) noexcept {
        std::uint32_t reg_index = irq_id / 32;
        std::uint32_t bit_offset = irq_id % 32;

        volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
            gicd::ICENABLER_BASE + reg_index * 4);
        *reg = (1U << bit_offset);
    }

    // Route an SPI (irq_id >= 32) to a set of CPUs (bit 0 = core 0, ...).
    // ITARGETSR is byte-indexed: one target byte per interrupt.
    void set_target(std::uint32_t irq_id, std::uint8_t cpu_mask) noexcept {
        std::uint32_t reg_index = irq_id / 4;
        std::uint32_t byte_offset = irq_id % 4;

        volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
            gicd::ITARGETSR_BASE + reg_index * 4);

        std::uint32_t val = *reg;
        val &= ~(0xFFU << (byte_offset * 8));
        val |= (static_cast<std::uint32_t>(cpu_mask) << (byte_offset * 8));
        *reg = val;
    }

    void set_priority(std::uint32_t irq_id, std::uint8_t priority) noexcept {
        std::uint32_t reg_index = irq_id / 4;
        std::uint32_t byte_offset = irq_id % 4;

        volatile auto* reg = reinterpret_cast<volatile std::uint32_t*>(
            gicd::IPRIORITYR_BASE + reg_index * 4);

        std::uint32_t val = *reg;
        val &= ~(0xFFU << (byte_offset * 8));
        val |= (static_cast<std::uint32_t>(priority) << (byte_offset * 8));
        *reg = val;
    }

    [[nodiscard]] std::uint32_t acknowledge() noexcept {
        return gicc::IAR::read() & 0x3FF;
    }

    [[nodiscard]] std::uint32_t acknowledge_raw() noexcept {
        return gicc::IAR::read();
    }

    void send_sgi(std::uint32_t cpu_mask, std::uint32_t sgi_id) noexcept {
        __asm__ volatile("dsb" ::: "memory");
        gicd::SGIR::write(((cpu_mask & 0xFF) << 16) | (sgi_id & 0xF));
    }

    void end_of_interrupt(std::uint32_t irq_id) noexcept {
        gicc::EOIR::write(irq_id);
    }

    [[nodiscard]] std::uint32_t get_num_irqs() const noexcept {
        return num_irqs_;
    }

private:
    std::uint32_t num_irqs_;
};

inline Gic gic;

namespace cpu {
    inline void enable_irq() noexcept {
        asm volatile("cpsie i" ::: "memory");
    }

    inline void disable_irq() noexcept {
        asm volatile("cpsid i" ::: "memory");
    }

    inline void enable_fiq() noexcept {
        asm volatile("cpsie f" ::: "memory");
    }

    inline void disable_fiq() noexcept {
        asm volatile("cpsid f" ::: "memory");
    }

    inline void enable_interrupts() noexcept {
        asm volatile("cpsie if" ::: "memory");
    }

    inline void disable_interrupts() noexcept {
        asm volatile("cpsid if" ::: "memory");
    }
}

} // namespace gic

#endif // GIC_HPP
