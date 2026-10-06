/**
 * @file  bcm2837.hpp
 * @brief BCM2837 / BCM2710A1 (Raspberry Pi Zero 2W, Pi 3) MMIO map — AArch32.
 *
 * The Pi Zero 2W has NO GIC. Two blocks matter for this µOS++ SMP port:
 *
 *   1. "Legacy" peripheral block @ 0x3F000000 (VideoCore peripheral bus
 *      0x7E000000 aliased to ARM-physical 0x3F000000): PL011 UART0, GPIO,
 *      system timer, the legacy ARM interrupt controller.
 *
 *   2. "ARM local peripherals" @ 0x40000000: per-core routing for the ARM
 *      generic-timer interrupts, the 4 per-core mailboxes (used here as the
 *      cross-core reschedule IPI), and the secondary-core spin-table
 *      (mailbox 3), plus the local-timer.
 *
 * All addresses are ARM-physical (post GPU-firmware, VA==PA identity map).
 */
#pragma once

#include <cstdint>

namespace bcm {

// --- peripheral (legacy) block --------------------------------------------
inline constexpr std::uint32_t PERIPH_BASE = 0x3F000000u;

// PL011 UART0 (GPIO14/15, pins 8/10 on the header)
inline constexpr std::uint32_t UART0_BASE = PERIPH_BASE + 0x201000u;
namespace pl011 {
inline constexpr std::uint32_t DR     = UART0_BASE + 0x00u; // data
inline constexpr std::uint32_t FR     = UART0_BASE + 0x18u; // flag
inline constexpr std::uint32_t IBRD   = UART0_BASE + 0x24u; // integer baud
inline constexpr std::uint32_t FBRD   = UART0_BASE + 0x28u; // fractional baud
inline constexpr std::uint32_t LCRH   = UART0_BASE + 0x2Cu; // line control
inline constexpr std::uint32_t CR     = UART0_BASE + 0x30u; // control
inline constexpr std::uint32_t IMSC   = UART0_BASE + 0x38u; // interrupt mask
inline constexpr std::uint32_t ICR    = UART0_BASE + 0x44u; // interrupt clear
// FR bits
inline constexpr std::uint32_t FR_TXFF = 1u << 5;           // TX FIFO full
inline constexpr std::uint32_t FR_RXFE = 1u << 4;           // RX FIFO empty
inline constexpr std::uint32_t FR_BUSY = 1u << 3;           // UART busy
}

// GPIO (for the ACT LED and UART alt-function select)
inline constexpr std::uint32_t GPIO_BASE   = PERIPH_BASE + 0x200000u;
namespace gpio {
inline constexpr std::uint32_t GPFSEL0  = GPIO_BASE + 0x00u; // fn select, 10 pins/reg
inline constexpr std::uint32_t GPSET0   = GPIO_BASE + 0x1Cu;
inline constexpr std::uint32_t GPCLR0   = GPIO_BASE + 0x28u;
}

// --- ARM local peripherals block ------------------------------------------
inline constexpr std::uint32_t LOCAL_BASE = 0x40000000u;
namespace local {
// per-core generic-timer interrupt routing control (bit0 nCNTPSIRQ,
// bit1 nCNTPNSIRQ, bit2 nCNTHPIRQ, bit3 nCNTVIRQ)
inline constexpr std::uint32_t TIMER_IRQCNTL0 = LOCAL_BASE + 0x40u; // +4*core
// per-core mailbox interrupt control (bit0..3 = mailbox 0..3 IRQ enable)
inline constexpr std::uint32_t MBOX_IRQCNTL0  = LOCAL_BASE + 0x50u; // +4*core
// per-core IRQ source (bit1 CNTPNSIRQ tick; bits4..7 = mailbox 0..3)
inline constexpr std::uint32_t IRQ_SOURCE0    = LOCAL_BASE + 0x60u; // +4*core
// per-core mailbox write-SET (trigger)   : base + 0x10*core + 4*mbox
inline constexpr std::uint32_t MBOX_SET0      = LOCAL_BASE + 0x80u;
// per-core mailbox write-CLEAR / read    : base + 0x10*core + 4*mbox
inline constexpr std::uint32_t MBOX_RDCLR0    = LOCAL_BASE + 0xC0u;

// IRQ-source bits
inline constexpr std::uint32_t SRC_CNTPNSIRQ = 1u << 1;  // physical timer (non-secure)
inline constexpr std::uint32_t SRC_CNTPSIRQ  = 1u << 0;  // physical timer (secure)
inline constexpr std::uint32_t SRC_MBOX0     = 1u << 4;  // mailbox 0 (our IPI)

// timer routing bits
inline constexpr std::uint32_t ROUTE_CNTPNSIRQ = 1u << 1;
inline constexpr std::uint32_t ROUTE_CNTPSIRQ  = 1u << 0;

// mailbox 0 is used as the reschedule IPI; mailbox 3 is the spin-table
inline constexpr std::uint32_t IPI_MBOX  = 0u;
inline constexpr std::uint32_t SPIN_MBOX = 3u;

// GPU (legacy-controller) interrupt routing:
//   bits[1:0] = core that takes GPU IRQs, bits[3:2] = core that takes GPU FIQs.
// One core takes ALL of them; there is no per-IRQ affinity on BCM2837.
inline constexpr std::uint32_t GPU_IRQ_ROUTING = LOCAL_BASE + 0x0Cu;

// IRQ-source bit 8: an enabled GPU interrupt is pending on this core.
inline constexpr std::uint32_t SRC_GPU = 1u << 8;
}

// --- legacy ARM interrupt controller (the "GPU" IRQs, 0..63) ----------------
// Enable/disable registers are write-1-to-act and do NOT read-modify-write, so
// setting one bit never disturbs another core's enables and needs no lock.
namespace armirq {
inline constexpr std::uint32_t BASE          = PERIPH_BASE + 0xB200u;
inline constexpr std::uint32_t BASIC_PENDING = BASE + 0x00u;
inline constexpr std::uint32_t PENDING1      = BASE + 0x04u; // IRQ 0..31
inline constexpr std::uint32_t PENDING2      = BASE + 0x08u; // IRQ 32..63
inline constexpr std::uint32_t FIQ_CONTROL   = BASE + 0x0Cu;
inline constexpr std::uint32_t ENABLE1       = BASE + 0x10u;
inline constexpr std::uint32_t ENABLE2       = BASE + 0x14u;
inline constexpr std::uint32_t ENABLE_BASIC  = BASE + 0x18u;
inline constexpr std::uint32_t DISABLE1      = BASE + 0x1Cu;
inline constexpr std::uint32_t DISABLE2      = BASE + 0x20u;
inline constexpr std::uint32_t DISABLE_BASIC = BASE + 0x24u;
}

// --- Synopsys DesignWare DWC2 OTG core --------------------------------------
inline constexpr std::uint32_t USB_BASE = PERIPH_BASE + 0x980000u;
inline constexpr unsigned      IRQ_USB  = 9u;  // GPU IRQ 9

// --- tiny register accessors ----------------------------------------------
inline void     mmio_write(std::uint32_t addr, std::uint32_t v) noexcept {
    *reinterpret_cast<volatile std::uint32_t*>(addr) = v;
}
inline std::uint32_t mmio_read(std::uint32_t addr) noexcept {
    return *reinterpret_cast<volatile std::uint32_t*>(addr);
}

inline void dsb() noexcept { __asm__ volatile("dsb sy" ::: "memory"); }
inline void isb() noexcept { __asm__ volatile("isb" ::: "memory"); }
inline void sev() noexcept { __asm__ volatile("sev" ::: "memory"); }

} // namespace bcm
