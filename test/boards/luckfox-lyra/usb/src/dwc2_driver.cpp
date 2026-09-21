/**
 * @file dwc2_driver.cpp
 * @brief C++23 driver implementation for DWC2 USB Controller on RK3506.
 */

#include "dwc2_driver.hpp"
#include "dwc2_gadget.hpp"
#include <stddef.h>

// External bare-metal assembly/C library hooks for caches and delays
extern "C" {
    void invalidate_dcache_range(uint32_t start, uint32_t stop);
    void flush_dcache_range(uint32_t start, uint32_t stop);
    void mdelay(unsigned long ms);
    void udelay(unsigned long us);
    void uart_puts(const char* s);
    int dwc2_gadget_probe();
}

namespace rk3506::usb::dwc2 {

/* EP0 setup-packet DMA target — lives in the Normal-NC .dma_nc window. */
alignas(64) uint8_t Driver::setup_buffer_[64] __attribute__((section(".dma_nc")));

// Gadget instance (dwc2_gadget.cpp); speed must be updated on enumeration
// or the composite framework serves full-speed descriptors to a HS host.
extern struct usb_gadget dwc2_gadget_instance;

// Memory barrier instructions
static inline void dsb() noexcept { asm volatile("dsb" : : : "memory"); }
static inline void dmb() noexcept { asm volatile("dmb" : : : "memory"); }
static inline void isb() noexcept { asm volatile("isb" : : : "memory"); }

Driver::Driver() noexcept
    : regs_(get_regs()),
      state_(rk3506::usb::State::disconnected),
      address_(0),
      connected_(false) {
    // Zero out setup buffer
    for (int i = 0; i < 64; ++i) {
        setup_buffer_[i] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        pending_in_[i]  = {nullptr, nullptr};
        pending_out_[i] = {nullptr, nullptr};
    }
}

void Driver::set_pending_request(uint8_t ep_num, bool is_in,
                                 struct usb_ep* ep, struct usb_request* req) noexcept {
    if (ep_num >= 16) return;
    if (is_in) {
        pending_in_[ep_num] = {ep, req};
    } else {
        pending_out_[ep_num] = {ep, req};
    }
}

void Driver::complete_pending(uint8_t ep_num, bool is_in) noexcept {
    PendingXfer& slot = is_in ? pending_in_[ep_num] : pending_out_[ep_num];
    struct usb_ep* ep = slot.ep;
    struct usb_request* req = slot.req;

    // Clear the slot before the callback: complete() commonly re-queues,
    // which re-fills the slot via set_pending_request().
    slot = {nullptr, nullptr};

    if (!req) return;

    if (is_in) {
        // IN transfer complete: entire buffer was transmitted
        req->actual = req->length;
    } else {
        // OUT transfer complete: actual = programmed size - residue in doeptsiz
        uint32_t residue = regs_->out_endp[ep_num].doeptsiz & epsiz::DOEPT_SIZ_XFER_SIZE_MAX_EP;
        req->actual = (residue <= req->length) ? (req->length - residue) : req->length;
        // Discard stale cache lines before the callback reads DMA-written data
        uint32_t start_addr = reinterpret_cast<uint32_t>(req->buf);
        invalidate_dcache_range(start_addr, start_addr + req->length);
    }

    req->status = 0;
    if (req->complete) {
        req->complete(ep, req);
    }
}

DriverStatus Driver::initialize() noexcept {
    // Called from main_entry and again via ops->udc_start when the composite
    // driver binds; the hardware must only be brought up once.
    if (initialized_) {
        uart_puts("DWC2: already initialized, skipping re-init.\n");
        return DriverStatus::OK;
    }
    uart_puts("DWC2: Starting initialization...\n");

    // 1. Clock Gating (CRU_GATE_CON07)
    // Address: 0xFF9A081C. Bits: 11 (pclk_usbphy), 7 (clk_usbotg0_adp), 6 (hclk_usbotg0_pmu), 5 (hclk_usbotg0)
    // Mask = 0x08E0 (Upper 16 bits set mask, lower 16 bits set to 0 to enable)
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x081CU) = (0x08E0U << 16) | 0x0000U;
    dmb();

    // 2. Soft Reset Assertion
    // CRU_SOFTRST_CON07 (0xFF9A0A1C): Bits 13 (resetn_usbphy_otg0), 12 (resetn_usbphy_por), 11 (presetn_usbphy), 7 (resetn_usbotg0_adp), 5 (hresetn_usbotg0)
    // Mask = 0x38A0. Write 1 to assert reset.
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0A1CU) = (0x38A0U << 16) | 0x38A0U;
    
    // CRU_SOFTRST_CON09 (0xFF9A0A24): Bit 0 (resetn_usbotg0_utmi). Write 1 to assert reset.
    // Mask = 0x0001
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0A24U) = (0x0001U << 16) | 0x0001U;
    
    dmb();
    mdelay(10); // Allow resets to propagate

    // 3. Soft Reset Release (Write 0 to de-assert)
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0A1CU) = (0x38A0U << 16) | 0x0000U;
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0A24U) = (0x0001U << 16) | 0x0000U;
    dmb();
    mdelay(10);

    // 3b. Un-gate the USB PHY 24 MHz reference clock (clk_ref_usbphy_top).
    // Per SDK clk-rk3506.c: CRU_GATE_CON01 (0xFF9A0804) bit 4, parent xin24m.
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0804U) = (0x0010U << 16) | 0x0000U;

    // 3c. Un-gate the DMA-to-DDR bridge (aclk/pclk_dma2ddr, GATE_CON08
    // bits 0/1 per SDK clk-rk3506.c; Linux keeps it CLK_IGNORE_UNUSED).
    // The DWC2 AHB master reaches DDR through this bridge: with it gated,
    // the core's DMA writes are silently dropped (doepdma advances, SUPCNT
    // decrements, but nothing lands in RAM) while register access via
    // hclk_usbotg0 still works.
    *reinterpret_cast<volatile uint32_t*>(CRU_BASE + 0x0820U) = (0x0003U << 16) | 0x0000U;
    dmb();

    // 3d. Open the DDR firewall for bus masters. The RK3506 boots with the
    // DDR firewall dropping DMA writes from unlisted masters (reads as 0,
    // no fault) while secure CPU accesses pass. U-Boot SPL arch_cpu_init()
    // clears per-master bytes in FW_DDR_MSTx (0xFF5F0020..0x2C) for the
    // masters it uses (sdmmc bits[15:8], fspi bits[23:16] of MST1) but
    // never for USB. Clear all master bytes so the DWC2 DMA can reach DDR.
    for (uint32_t off = 0x20U; off <= 0x2CU; off += 4U) {
        *reinterpret_cast<volatile uint32_t*>(0xFF5F0000U + off) = 0x00000000U;
    }
    dmb();

    // 4. Force OTG PHY to Peripheral Device mode via GRF overrides.
    // Field layout per SDK phy-rockchip-inno-usb2.c rk3506_phy_cfgs (OTG port):
    //   GRF 0x0060 bits [8:0]  = phy_sus (0x1D1 = suspended, 0 = running)
    //   GRF 0x0060 bit 9       = iddig_en  (software ID override enable)
    //   GRF 0x0060 bit 10      = iddig_output (ID = 1 -> device mode)
    //   GRF 0x0060 bits [15:14]= bvalid_grf_con (0b11 = force BVALID = 1)
    // Clear the whole phy_sus field (a stray bit here suspends the PHY) and
    // set the ID/BVALID overrides. Mask = 0xC7FF, Val = 0xC600.
    *reinterpret_cast<volatile uint32_t*>(GRF_BASE + 0x0060U) = (0xC7FFU << 16) | 0xC600U;
    dmb();

    uart_puts("DWC2: Clock and PHY overrides complete. Resetting IP core...\n");

    // 5. Hard Reset the IP core
    if (reset() != DriverStatus::OK) {
        uart_puts("DWC2: Core reset failed!\n");
        return DriverStatus::ERROR;
    }

    // 6. Setup internal FIFO RAM divisions
    configure_fifos();

    // 7. AHB Bus Configuration (gahbcfg)
    // Enable DMA mode (MODE_DMA), select burst type INCR4, enable global interrupts (GBL_INT_UNMASK)
    regs_->gahbcfg = gahbcfg::MODE_DMA | gahbcfg::BURST_INCR4 | gahbcfg::GBL_INT_UNMASK;

    // 7b. USB Core Configuration (gusbcfg): force device mode and select the
    // 16-bit UTMI+ PHY interface (turnaround time 5 for 16-bit).
    regs_->gusbcfg = (regs_->gusbcfg & ~gusbcfg::USBTRDTIM_MASK)
                   | gusbcfg::FORCE_DEVICE_MODE
                   | gusbcfg::PHYIF_16BIT
                   | gusbcfg::USBTRDTIM(5);
    dmb();
    mdelay(30); // Force-mode change takes effect after ~25 ms

    // 8. Device Speed configuration (make SPEED=fs forces Full-Speed on
    // both controllers — e.g. for marginal cabling; bulk MPS drops to 64)
#ifdef USB_FORCE_FS
    regs_->dcfg = (regs_->dcfg & ~0x3U) | dcfg::DEV_SPEED_FULL_SPEED_20;
#else
    regs_->dcfg = (regs_->dcfg & ~0x3U) | dcfg::DEV_SPEED_HIGH_SPEED_20;
#endif

    // 9. Register GIC routing
#ifndef USB_UNDER_RTOS
    // Standalone firmware: route INTID 106 to core 0 and take real IRQs.
    configure_gic();
#else
    // Under StateOS the controller is POLLED from the USB task: enabling
    // the (level-triggered) INTID 106 with no kernel handler would livelock
    // the core in the IRQ dispatcher. The GIC stays owned by the kernel.
#endif

    // 10. Enable Core Level Interrupts
    enable_interrupts();

    // 11. Arm Control Endpoint (EP0) to receive Setup packets
    start_rx(0, setup_buffer_, 8);

    // 12. Register UDC gadget to the composite core list
    dwc2_gadget_probe();

    initialized_ = true;
    uart_puts("DWC2: Driver initialized and armed.\n");
    return DriverStatus::OK;
}

DriverStatus Driver::reset() noexcept {
    // Wait for AHB master to be idle before soft reset
    uint32_t count = 0;
    while (!(regs_->grstctl & grstctl::AHB_MASTER_IDLE)) {
        mdelay(1);
        if (++count > 500) {
            uart_puts("DWC2: Timeout waiting for AHB idle!\n");
            return DriverStatus::TIMEOUT;
        }
    }

    // Assert soft reset
    regs_->grstctl |= grstctl::CORE_SOFT_RESET;
    dmb();

    // Poll until core reset bit is cleared
    count = 0;
    while (regs_->grstctl & grstctl::CORE_SOFT_RESET) {
        udelay(10);
        if (++count > 1000) {
            uart_puts("DWC2: Timeout waiting for Core Reset release!\n");
            return DriverStatus::TIMEOUT;
        }
    }
    
    mdelay(5); // Stabilize
    return DriverStatus::OK;
}

void Driver::configure_fifos() noexcept {
    // Partitioning based on Rockchip SDK allocations (values in 32-bit words)
    // Rx FIFO size = 280 words (1120 bytes)
    regs_->grxfsiz = 280;

    // Non-Periodic Tx FIFO size = 256 words (1024 bytes)
    // Positioned immediately after Rx FIFO (offset 280)
    regs_->gnptxfsiz = (256U << 16) | 280U;

    // Allocate Periodic/Logical Tx FIFOs (dieptxf) for EP1 - EP5
    // Each FIFO requires a unique starting address offset in RAM
    uint32_t current_offset = 280 + 256;
    
    // EP1-TX (Bulk IN): 256 words
    regs_->dieptxf[0] = (256U << 16) | current_offset;
    current_offset += 256;

    // EP2-TX (Bulk IN): 128 words
    regs_->dieptxf[1] = (128U << 16) | current_offset;
    current_offset += 128;

    // EP3-TX: 128 words
    regs_->dieptxf[2] = (128U << 16) | current_offset;
    current_offset += 128;

    // EP4-TX: 64 words
    regs_->dieptxf[3] = (64U << 16) | current_offset;
    current_offset += 64;

    // EP5-TX: 32 words
    regs_->dieptxf[4] = (32U << 16) | current_offset;
    dmb();
}

void Driver::enable_interrupts() noexcept {
    // Clear all pending status
    regs_->gintsts = 0xFFFFFFFFU;

    // Mask interrupts we want to process
    regs_->gintmsk = gintsts::INT_ENUMDONE | 
                     gintsts::INT_RESET | 
                     gintsts::INT_SUSPEND | 
                     gintsts::INT_OUT_EP | 
                     gintsts::INT_IN_EP;

    // Enable IN and OUT endpoint common masks
    regs_->diepmsk = epint::TRANSFER_DONE | epint::AHB_ERROR;
    regs_->doepmsk = epint::TRANSFER_DONE | epint::CTRL_OUT_EP_SETUP_PHASE_DONE | epint::AHB_ERROR;

    // Route EP0 IN/OUT into daint: without daintmsk bits set, daint & daintmsk
    // is always 0 and no endpoint interrupt (incl. SETUP) is ever dispatched.
    regs_->daintmsk = daint::IN_EP(0) | daint::OUT_EP(0);
    dmb();
}

void Driver::configure_gic() noexcept {
    // RK3506 GIC-400 base addresses
    constexpr uintptr_t GIC_DIST_BASE = 0xFF581000U;
    constexpr uintptr_t GIC_CPU_BASE  = 0xFF582000U;

    // Registers offset constants
    constexpr uintptr_t GICD_ISENABLER_OFF = 0x0100U;
    constexpr uintptr_t GICD_IPRIORITY_OFF = 0x0400U;
    constexpr uintptr_t GICD_ITARGETS_OFF  = 0x0800U;
    constexpr uintptr_t GICD_ICFGR_OFF     = 0x0C00U;

    // USB0 interrupt ID on RK3506 is SPI 74 (GIC ID = 74 + 32 = 106)
    constexpr uint32_t USB0_INTID = 106;

    auto* gicd_ctlr = reinterpret_cast<volatile uint32_t*>(GIC_DIST_BASE + 0x00U);
    auto* gicc_ctlr = reinterpret_cast<volatile uint32_t*>(GIC_CPU_BASE  + 0x00U);
    auto* gicc_pmr  = reinterpret_cast<volatile uint32_t*>(GIC_CPU_BASE  + 0x04U);

    // Enable GIC Distributor (Group 0 & Group 1)
    *gicd_ctlr |= 0x03U;

    // Enable GIC CPU Interface
    *gicc_ctlr |= 0x01U;

    // Set Priority Mask Register to allow all interrupts
    *gicc_pmr = 0xFFU;

    // Enable USB0 Interrupt in distributor
    volatile uint32_t* isenabler = reinterpret_cast<volatile uint32_t*>(
        GIC_DIST_BASE + GICD_ISENABLER_OFF + (USB0_INTID / 32U) * 4U);
    *isenabler = (1U << (USB0_INTID % 32U));

    // Set priority to default medium (0x80)
    volatile uint8_t* ipriority = reinterpret_cast<volatile uint8_t*>(
        GIC_DIST_BASE + GICD_IPRIORITY_OFF);
    ipriority[USB0_INTID] = 0x80U;

    // Route to CPU core 0
    volatile uint8_t* itargets = reinterpret_cast<volatile uint8_t*>(
        GIC_DIST_BASE + GICD_ITARGETS_OFF);
    itargets[USB0_INTID] = 0x01U; // Core 0 bitmask

    // Set interrupt to edge-triggered (binary pattern: 10 on 2-bit field)
    volatile uint32_t* icfgr = reinterpret_cast<volatile uint32_t*>(
        GIC_DIST_BASE + GICD_ICFGR_OFF + (USB0_INTID / 16U) * 4U);
    uint32_t bit_shift = (USB0_INTID % 16U) * 2U;
    *icfgr = (*icfgr & ~(3U << bit_shift)) | (2U << bit_shift);

    dsb();
    isb();
    uart_puts("DWC2: GIC SPI 74 (GIC ID 106) configured to CPU Core 0.\n");
}

void Driver::set_address(uint8_t address) noexcept {
    address_ = address;
    regs_->dcfg = (regs_->dcfg & ~dcfg::DCFG_DEVADDR_MASK) | dcfg::DEVICE_ADDRESS(address);
    dmb();
}

void Driver::start_tx(uint8_t ep_num, const void* buf, uint32_t len, uint8_t fifo_num) noexcept {
    // 1. D-cache Flush (clean) to force CPU writes back to physical RAM
    uint32_t start_addr = reinterpret_cast<uint32_t>(buf);
    uint32_t stop_addr  = start_addr + len;
    flush_dcache_range(start_addr, stop_addr);

    // 2. Clear & Flush tx FIFO
    regs_->grstctl = grstctl::TX_FIFO_NUMBER(fifo_num);
    regs_->grstctl |= grstctl::TX_FIFO_FLUSH;
    while (regs_->grstctl & grstctl::TX_FIFO_FLUSH) {
        __asm__ volatile("nop");
    }

    // 3. Write physical buffer address
    regs_->in_endp[ep_num].diepdma = start_addr;

    // 4. Set packet count and total transfer size
    // For Bulk endpoints, maxpacket is typically 512 in High Speed
    uint32_t max_packet = (ep_num == 0) ? 64U : 512U;
    uint32_t pktcnt = (len == 0) ? 1U : ((len - 1U) / max_packet + 1U);

    regs_->in_endp[ep_num].dieptsiz = epsiz::DIEPT_SIZ_PKT_CNT(pktcnt) | epsiz::DIEPT_SIZ_XFER_SIZE(len);

    // 5. Setup control bits (FIFO assignment) and enable transmit DMA
    uint32_t ctrl = regs_->in_endp[ep_num].diepctl;
    ctrl &= epctl::DIEPCTL_TX_FIFO_NUM_MASK;
    ctrl |= epctl::DIEPCTL_TX_FIFO_NUM(fifo_num);

    regs_->in_endp[ep_num].diepctl = epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK | ctrl;
    dmb();
}

void Driver::start_rx(uint8_t ep_num, void* buf, uint32_t len) noexcept {
    // 1. OUT transfers must be armed with a size rounded UP to a multiple
    // of the endpoint MPS: the core refuses/mishandles receptions armed
    // with less than one max packet. (Callers' buffers must accommodate
    // the rounded size; the completion path derives actual = len - residue.)
    uint32_t max_packet = (ep_num == 0) ? 64U : 512U;
    uint32_t pktcnt = (len == 0) ? 1U : ((len - 1U) / max_packet + 1U);
    if (ep_num != 0) {
        len = pktcnt * max_packet;
    }

    // 2. D-cache Invalidate to discard CPU cached values
    uint32_t start_addr = reinterpret_cast<uint32_t>(buf);
    uint32_t stop_addr  = start_addr + len;
    invalidate_dcache_range(start_addr, stop_addr);

    // 3. Write physical buffer address
    regs_->out_endp[ep_num].doepdma = start_addr;

    if (ep_num == 0) {
        // EP0 OUT must always be able to accept SETUP packets: program the
        // SETUP count field (back-to-back setups) alongside the data size.
        regs_->out_endp[0].doeptsiz = epsiz::DOEPT_SIZ_SUP_CNT(3)
                                    | epsiz::DIEPT_SIZ_PKT_CNT(pktcnt)
                                    | epsiz::DIEPT_SIZ_XFER_SIZE(len);
    } else {
        regs_->out_endp[ep_num].doeptsiz = epsiz::DIEPT_SIZ_PKT_CNT(pktcnt) | epsiz::DIEPT_SIZ_XFER_SIZE(len);
    }

    // 4. Enable OUT endpoint and clear NAK to accept packets
    uint32_t ctrl = regs_->out_endp[ep_num].doepctl;
    regs_->out_endp[ep_num].doepctl = epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK | ctrl;
    dmb();
}

void Driver::stall(uint8_t ep_num, bool is_in, bool stall_ep) noexcept {
    if (is_in) {
        if (stall_ep) {
            regs_->in_endp[ep_num].diepctl |= epctl::DEPCTL_STALL;
        } else {
            regs_->in_endp[ep_num].diepctl &= ~epctl::DEPCTL_STALL;
        }
    } else {
        if (stall_ep) {
            regs_->out_endp[ep_num].doepctl |= epctl::DEPCTL_STALL;
        } else {
            regs_->out_endp[ep_num].doepctl &= ~epctl::DEPCTL_STALL;
        }
    }
    dmb();
}

bool Driver::is_high_speed() const noexcept {
    // Read speed field (bits 1-2) from Device Status (dsts)
    // 0 = High Speed, 1 = Full Speed (30/60MHz), 2 = Low Speed, 3 = Full Speed (48MHz)
    return (regs_->dsts & 0x6U) == 0;
}

void Driver::handle_interrupt() noexcept {
    uint32_t status = regs_->gintsts;
    uint32_t mask = regs_->gintmsk;

    if (!(status & mask)) return; // No unmasked interrupt pending

    // 1. Bus Reset detected
    if (status & gintsts::INT_RESET) {
        regs_->gintsts = gintsts::INT_RESET; // Clear
        set_address(0);
        state_ = rk3506::usb::State::default_state;

        // NAK all OUT endpoints, then restore the EP0-only interrupt routing
        // (SET_CONFIGURATION re-adds bulk endpoints via ep_enable).
        for (int i = 0; i < 16; ++i) {
            regs_->out_endp[i].doepctl |= epctl::DEPCTL_SNAK;
        }
        regs_->daintmsk = daint::IN_EP(0) | daint::OUT_EP(0);

        uart_puts("DWC2: USB Bus Reset occurred.\n");
        // Re-arm setup packet on EP0-OUT
        start_rx(0, setup_buffer_, 8);
    }

    // 2. Speed detection / Enumeration Done
    if (status & gintsts::INT_ENUMDONE) {
        regs_->gintsts = gintsts::INT_ENUMDONE;
        bool hs = is_high_speed();
        state_ = rk3506::usb::State::addressed;

        // Publish the negotiated speed: composite picks fs vs hs descriptor
        // sets (and thus bulk MPS 64 vs 512) from gadget->speed.
        dwc2_gadget_instance.speed = hs ? USB_SPEED_HIGH : USB_SPEED_FULL;

        // Program EP0 max packet size (0b00 = 64 bytes for both HS and FS)
        // and make EP0 active in both directions.
        regs_->in_endp[0].diepctl = (regs_->in_endp[0].diepctl & ~epctl::DEPCTL_MPS_MASK)
                                  | epctl::DEPCTL0_MPS_64 | epctl::DEPCTL_USBACTEP;
        regs_->out_endp[0].doepctl |= epctl::DEPCTL_USBACTEP | epctl::DEPCTL_CNAK;
        dmb();

        uart_puts(hs ? "DWC2: Speed Negotiated: HIGH-SPEED (480 Mbps).\n"
                     : "DWC2: Speed Negotiated: FULL-SPEED (12 Mbps).\n");
    }

    // 3. Core Suspend
    if (status & gintsts::INT_SUSPEND) {
        regs_->gintsts = gintsts::INT_SUSPEND;
        state_ = rk3506::usb::State::suspended;
        uart_puts("DWC2: Bus suspended.\n");
    }

    // 4. IN Endpoints interrupts (logical Tx completed)
    if (status & gintsts::INT_IN_EP) {
        process_ep_in_interrupt();
    }

    // 5. OUT Endpoints interrupts (logical Rx completed or Setup arrived)
    if (status & gintsts::INT_OUT_EP) {
        process_ep_out_interrupt();
    }
}

void Driver::process_ep_in_interrupt() noexcept {
    // Read Device All Endpoints Interrupt (daint)
    uint32_t ep_intr = regs_->daint & regs_->daintmsk & 0xFFFFU; // Bits 0-15 correspond to IN

    for (uint8_t ep_num = 0; ep_intr != 0; ++ep_num) {
        if (ep_intr & (1U << ep_num)) {
            uint32_t ep_status = regs_->in_endp[ep_num].diepint;
            regs_->in_endp[ep_num].diepint = ep_status; // Clear

            if (ep_status & epint::TRANSFER_DONE) {
                // Handle Bulk IN complete (e.g. notify gadget framework)
                if (ep_num == 0) {
                    // EP0 IN completed (data or status stage). Notify any
                    // composite-queued request, then arm for the next setup.
                    complete_pending(0, true);
                    state_ = (address_ != 0) ? rk3506::usb::State::addressed
                                             : rk3506::usb::State::default_state;
                    start_rx(0, setup_buffer_, 8);
                } else {
                    // Bulk/interrupt IN complete: notify gadget framework
                    complete_pending(ep_num, true);
                }
            }
            ep_intr &= ~(1U << ep_num);
        }
    }
}

void Driver::process_ep_out_interrupt() noexcept {
    // DAINT bits 16-31 correspond to OUT endpoints
    uint32_t ep_intr = (regs_->daint & regs_->daintmsk) >> 16;

    for (uint8_t ep_num = 0; ep_intr != 0; ++ep_num) {
        if (ep_intr & (1U << ep_num)) {
            uint32_t ep_status = regs_->out_endp[ep_num].doepint;
            regs_->out_endp[ep_num].doepint = ep_status; // Clear

            if (ep_num == 0) {
                // Setup packet arrived
                if (ep_status & epint::CTRL_OUT_EP_SETUP_PHASE_DONE) {
                    handle_ep0_setup();
                }
                // EP0 OUT Transfer completed (Control Data received or the
                // status-stage ZLP of an IN control transfer)
                else if (ep_status & epint::TRANSFER_DONE) {
                    complete_pending(0, false);
                    // The completed transfer consumed the armed EP0-OUT
                    // buffer; re-arm it or the next SETUP has nowhere to
                    // land and the host times out into a bus reset.
                    start_rx(0, setup_buffer_, 8);
                }
            } else {
                if (ep_status & epint::TRANSFER_DONE) {
                    // Bulk/interrupt OUT complete: notify gadget framework
                    complete_pending(ep_num, false);
                }
            }
            ep_intr &= ~(1U << ep_num);
        }
    }
}

} // namespace rk3506::usb::dwc2

