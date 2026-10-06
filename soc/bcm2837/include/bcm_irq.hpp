/**
 * @file  bcm_irq.hpp
 * @brief BCM2837 interrupt glue — replaces the a7 port's gic.hpp.
 *
 * The Pi Zero 2W has no GIC. Two interrupt sources drive the SMP scheduler:
 *
 *   - the per-core ARM generic-timer PPI (CNTPNSIRQ), routed to its own core
 *     through the local-peripheral "Core N timer IRQ control" register, and
 *   - the per-core mailbox 0, used as the cross-core reschedule IPI, routed
 *     through "Core N mailbox IRQ control".
 *
 * The taken IRQ is identified by reading "Core N IRQ source". There is no
 * acknowledge/priority/EOI protocol like the GIC: the timer is re-armed by
 * writing CNTP_TVAL (done in os-core.cpp), and a mailbox is cleared by
 * writing its set bits back to the mailbox read/clear register.
 */
#pragma once

#include <cstdint>
#include "bcm2837.hpp"

namespace bcm_irq {

// Core index from MPIDR. The register is the same; only the access
// instruction differs between the two ISAs.
inline unsigned this_core() noexcept {
#if defined(__aarch64__)
    std::uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<unsigned>(mpidr) & 3u;
#else
    unsigned mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3u;
#endif
}

// Route this core's non-secure physical-timer interrupt (CNTPNSIRQ) to itself.
inline void route_timer_this_core() noexcept {
    using namespace bcm;
    const unsigned c = this_core();
    mmio_write(local::TIMER_IRQCNTL0 + 4u * c, local::ROUTE_CNTPNSIRQ);
}

// Enable mailbox-0 (the IPI) interrupt delivery to this core.
inline void enable_ipi_this_core() noexcept {
    using namespace bcm;
    const unsigned c = this_core();
    mmio_write(local::MBOX_IRQCNTL0 + 4u * c, 1u << local::IPI_MBOX);
}

// Read the pending IRQ source bits for a given core.
inline std::uint32_t source(unsigned core) noexcept {
    using namespace bcm;
    return mmio_read(local::IRQ_SOURCE0 + 4u * core);
}

// Fire the reschedule IPI at target core (writes mailbox-0 set register).
inline void send_ipi(unsigned core) noexcept {
    using namespace bcm;
    mmio_write(local::MBOX_SET0 + 0x10u * core + 4u * local::IPI_MBOX, 1u);
    dsb();
    sev();
}

// Acknowledge/clear this core's IPI mailbox (write-1-to-clear the set bits).
inline void ack_ipi(unsigned core) noexcept {
    using namespace bcm;
    mmio_write(local::MBOX_RDCLR0 + 0x10u * core + 4u * local::IPI_MBOX, ~0u);
    dsb();
}

// Send every GPU (legacy-controller) interrupt to one core. Call once, on the
// core that will service them, before enabling any individual GPU IRQ.
inline void route_gpu_irqs_to_core(unsigned core) noexcept {
    using namespace bcm;
    mmio_write(local::GPU_IRQ_ROUTING, (core & 3u) | ((core & 3u) << 2));
    dsb();
}

// Enable GPU IRQ `n` (0..63). Write-1-to-act: only bit n is touched.
inline void enable_gpu_irq(unsigned n) noexcept {
    using namespace bcm;
    mmio_write(n < 32u ? armirq::ENABLE1 : armirq::ENABLE2, 1u << (n & 31u));
    dsb();
}

// Disable GPU IRQ `n` (0..63). Write-1-to-act: only bit n is touched.
inline void disable_gpu_irq(unsigned n) noexcept {
    using namespace bcm;
    mmio_write(n < 32u ? armirq::DISABLE1 : armirq::DISABLE2, 1u << (n & 31u));
    dsb();
}

// True when GPU IRQ `n` is asserted (and enabled — the pending registers only
// report enabled sources).
inline bool gpu_irq_pending(unsigned n) noexcept {
    using namespace bcm;
    const std::uint32_t p = mmio_read(n < 32u ? armirq::PENDING1 : armirq::PENDING2);
    return (p & (1u << (n & 31u))) != 0u;
}

} // namespace bcm_irq
