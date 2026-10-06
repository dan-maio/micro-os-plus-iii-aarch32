/**
 * @file usb1_int3.cpp
 * @brief USB OTG1 (0xFF780000) DEVICE-mode gadget for smp_test_int3 —
 *        INTERRUPT-driven keystroke sink.
 *
 * Derived from the shared src/usb/usb1_dev.cpp (which stays untouched —
 * test-specific USB code lives in this subfolder per project policy).
 * Differences from the parent:
 *
 *   - PID 0x001B (int3 keystroke sink) instead of 0x001A (bridge port).
 *   - No polled owner loop: usb1i3_service() is called from the GIC IRQ
 *     path (port_external_irq_handler in main.cpp) — every bus reset,
 *     enumeration step, EP0 setup and EP1 transfer completion is an
 *     interrupt.
 *   - RX simplified to a single self-re-arming 512-byte read on bulk
 *     EP1-OUT: each completed packet is delivered to usb1i3_rx_packet()
 *     (strong hook in main.cpp — carries the PC keyboard events), then
 *     the read is re-armed immediately from the ISR. The streaming-sink
 *     machinery of the parent is not needed here.
 *   - Console output goes through app_console_write() (main.cpp routes
 *     it into the IRQ-driven TX ring) — never polled mid-ISR.
 *
 * The bring-up sequence in usb1i3_init() (VBUS switch off, CRU
 * clocks/resets, GRF_SOC_CON28 = 0xCE00, core soft reset, FIFO map) is
 * the parent's board-proven code, kept verbatim.
 */

#include <stdint.h>
#include "dwc2_regs.hpp"

extern "C" {
    void app_console_write(const char *s);           // main.cpp (TX ring)
    void invalidate_dcache_range(uint32_t start, uint32_t stop);
    void flush_dcache_range(uint32_t start, uint32_t stop);
    void mdelay(unsigned long ms);

    // Strong hook in main.cpp: one completed EP1-OUT packet (keystrokes).
    void usb1i3_rx_packet(const uint8_t* pkt, uint32_t len);
}

/// IN-transfer-complete hook (unused in stage A; stage B HID will use it).
extern "C" void usb1i3_tx_done(void) __attribute__((weak));
extern "C" void usb1i3_tx_done(void) {}

// Debug counters, readable by the app:
// [0]=service calls [1]=resets [2]=enumdones [3]=OUT-EP irqs [4]=SETUPs
// [5]=IN-EP irqs [6]=EP1-OUT completions [7]=last serviced GINTSTS
// [8]=last SETUP bytes 0-3 [9]=last SETUP bytes 4-7 [10]=stalls
// [11]=ep0 IN transfers started [12]=last DOEPDMA0 readback in setup
volatile uint32_t usb1i3_dbg[13];

/// Data memory barrier.
static inline void dmb() noexcept { asm volatile("dmb" : : : "memory"); }

namespace {

using namespace rk3506::usb::dwc2;

constexpr uintptr_t CRU1_BASE = 0xFF9A0000U;
constexpr uintptr_t GRF1_BASE = 0xFF288000U;

inline ControllerRegs* regs1() {
    return reinterpret_cast<ControllerRegs*>(USB_OTG1_BASE);
}
inline void masked_write(uintptr_t addr, uint32_t mask, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(addr) = (mask << 16) | val;
}

// ---- state -----------------------------------------------------------
// DMA-touched buffers must live in .dma_nc (Normal-NC window; the cache
// hooks are no-ops).
uint8_t setup_buf1[64] __attribute__((aligned(64), section(".dma_nc")));
uint8_t ep0_buf1[64]   __attribute__((aligned(64), section(".dma_nc")));
uint8_t cmd1_buf[512]  __attribute__((aligned(64), section(".dma_nc")));

bool     configured1   = false;
bool     tx_busy1      = false;
uint8_t  address1      = 0;
bool     rx1_pending   = false;   ///< an OUT read is armed
uint32_t rx1_armed     = 0;       ///< bytes armed for the in-flight OUT

// The data-OUT endpoint number. NOT hardcoded to 1: OTG1's DWC2 instance
// may implement EP1 as IN-only (board-found in smp_test_int3: DAINTMSK
// bit 17 would not stick — the stateos bridge only ever used EP1-IN on
// this controller). Probed at init from the DAINTMSK implemented-bits
// readback; the config descriptor is patched to match.
uint8_t  out_ep_num    = 1;
uint32_t daintmsk_impl = 0;       ///< implemented-endpoint probe result

// ---- descriptors (hardcoded; no strings so indexes stay 0) ------------
alignas(64) const uint8_t dev_desc1[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    0x07, 0x22,             // idVendor  0x2207 (Rockchip)
    0x1B, 0x00,             // idProduct 0x001B (int3 keystroke sink)
    0x00, 0x01,             // bcdDevice 1.00
    0, 0, 0,                // no strings
    1,                      // one configuration
};

#ifdef USB_FORCE_FS
constexpr uint32_t EP1_MPS = 64;
#else
constexpr uint32_t EP1_MPS = 512;
#endif

// config(9) + interface(9) + bulk-IN(7) + bulk-OUT(7) = 32 bytes
// Non-const: byte 27 (bulk-OUT bEndpointAddress) is patched at init with
// the probed OUT endpoint number.
alignas(64) uint8_t cfg_desc1[32] = {
    9, 0x02, 32, 0, 1, 1, 0, 0xC0, 50,
    9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    7, 0x05, 0x81, 0x02,
    static_cast<uint8_t>(EP1_MPS & 0xFF),
    static_cast<uint8_t>(EP1_MPS >> 8), 0x00, // EP1-IN bulk
    7, 0x05, 0x01, 0x02,
    static_cast<uint8_t>(EP1_MPS & 0xFF),
    static_cast<uint8_t>(EP1_MPS >> 8), 0x00, // EP1-OUT bulk (keystrokes)
};

// ---- low-level helpers -------------------------------------------------
void ep0_arm_setup() {
    invalidate_dcache_range(reinterpret_cast<uint32_t>(setup_buf1),
                            reinterpret_cast<uint32_t>(setup_buf1) + 64);
    auto* r = regs1();
    r->out_endp[0].doepdma = reinterpret_cast<uint32_t>(setup_buf1);
    r->out_endp[0].doeptsiz = epsiz::DOEPT_SIZ_SUP_CNT(3)
                            | epsiz::DIEPT_SIZ_PKT_CNT(1)
                            | epsiz::DIEPT_SIZ_XFER_SIZE(8);
    r->out_endp[0].doepctl = epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK
                           | r->out_endp[0].doepctl;
    dmb();
}

void ep_tx(uint8_t ep, const void* buf, uint32_t len) {
    if (ep == 0)
        usb1i3_dbg[11] = usb1i3_dbg[11] + 1;
    auto* r = regs1();
    uint32_t start = reinterpret_cast<uint32_t>(buf);
    flush_dcache_range(start, start + len);

    r->grstctl = grstctl::TX_FIFO_NUMBER(ep);
    r->grstctl |= grstctl::TX_FIFO_FLUSH;
    while (r->grstctl & grstctl::TX_FIFO_FLUSH) { __asm__ volatile("nop"); }

    uint32_t mps = (ep == 0) ? 64U : EP1_MPS;
    uint32_t pktcnt = (len == 0) ? 1U : ((len - 1U) / mps + 1U);
    r->in_endp[ep].diepdma = start;
    r->in_endp[ep].dieptsiz = epsiz::DIEPT_SIZ_PKT_CNT(pktcnt)
                            | epsiz::DIEPT_SIZ_XFER_SIZE(len);
    uint32_t ctrl = r->in_endp[ep].diepctl;
    ctrl &= epctl::DIEPCTL_TX_FIFO_NUM_MASK;
    ctrl |= epctl::DIEPCTL_TX_FIFO_NUM(ep);
    r->in_endp[ep].diepctl = epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK | ctrl;
    dmb();
}

void ep0_stall() {
    usb1i3_dbg[10] = usb1i3_dbg[10] + 1;
    auto* r = regs1();
    r->in_endp[0].diepctl  |= epctl::DEPCTL_STALL;
    r->out_endp[0].doepctl |= epctl::DEPCTL_STALL;
    ep0_arm_setup();
}

/// Arm one 512-byte keystroke read on the bulk-OUT endpoint (512-multiple
/// rule: an arm ending mid-packet hangs the controller — found on OTG0).
void ep1_arm_read() {
    auto* r = regs1();
    invalidate_dcache_range(reinterpret_cast<uint32_t>(cmd1_buf),
                            reinterpret_cast<uint32_t>(cmd1_buf) + sizeof cmd1_buf);
    r->out_endp[out_ep_num].doepdma  = reinterpret_cast<uint32_t>(cmd1_buf);
    r->out_endp[out_ep_num].doeptsiz = epsiz::DIEPT_SIZ_PKT_CNT(sizeof cmd1_buf / EP1_MPS)
                            | epsiz::DIEPT_SIZ_XFER_SIZE(sizeof cmd1_buf);
    r->out_endp[out_ep_num].doepctl |= epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK;
    rx1_pending = true;
    rx1_armed   = sizeof cmd1_buf;
    dmb();
}

// ---- EP0 setup handling -------------------------------------------------
void handle_setup1() {
    invalidate_dcache_range(reinterpret_cast<uint32_t>(setup_buf1),
                            reinterpret_cast<uint32_t>(setup_buf1) + 64);
    // SUPCNT auto-advances doepdma by 8 per SETUP; freshest packet is just
    // below the current readback (same scheme as the OTG0 driver)
    uint32_t base = reinterpret_cast<uint32_t>(setup_buf1);
    uint32_t dma_now = regs1()->out_endp[0].doepdma;
    const uint8_t* p = setup_buf1;
    if (dma_now >= base + 8U && dma_now <= base + sizeof(setup_buf1))
        p = reinterpret_cast<const uint8_t*>(dma_now - 8U);

    usb1i3_dbg[12] = dma_now;
    usb1i3_dbg[8] = static_cast<uint32_t>(p[0]) | (p[1] << 8)
                  | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    usb1i3_dbg[9] = static_cast<uint32_t>(p[4]) | (p[5] << 8)
                  | (p[6] << 16) | (static_cast<uint32_t>(p[7]) << 24);

    uint8_t  bmReq   = p[0];
    uint8_t  bReq    = p[1];
    uint16_t wValue  = static_cast<uint16_t>(p[2] | (p[3] << 8));
    uint16_t wLength = static_cast<uint16_t>(p[6] | (p[7] << 8));

    if ((bmReq & 0x60U) != 0) {       // only standard requests
        ep0_stall();
        return;
    }

    switch (bReq) {
    case 0x05:                        // SET_ADDRESS
        address1 = static_cast<uint8_t>(wValue & 0x7FU);
        regs1()->dcfg = (regs1()->dcfg & ~dcfg::DCFG_DEVADDR_MASK)
                      | dcfg::DEVICE_ADDRESS(address1);
        ep_tx(0, ep0_buf1, 0);        // status ZLP
        return;

    case 0x06: {                      // GET_DESCRIPTOR
        const uint8_t* src = nullptr;
        uint32_t len = 0;
        switch (wValue >> 8) {
        case 0x01: src = dev_desc1; len = sizeof(dev_desc1); break;
        case 0x02: src = cfg_desc1; len = sizeof(cfg_desc1); break;
        default: break;               // strings/qualifier -> stall
        }
        if (!src) { ep0_stall(); return; }
        if (len > wLength) len = wLength;
        for (uint32_t i = 0; i < len; ++i) ep0_buf1[i] = src[i];
        ep_tx(0, ep0_buf1, len);
        return;
    }

    case 0x09: {                      // SET_CONFIGURATION
        auto* r = regs1();
        uint32_t ctrl = epctl::DEPCTL_USBACTEP | epctl::DEPCTL_TYPE_BULK
                      | epctl::DEPCTL_SETD0PID
                      | (EP1_MPS & epctl::DEPCTL_MPS_MASK)
                      | epctl::DIEPCTL_TX_FIFO_NUM(1);
        r->in_endp[1].diepctl = ctrl;
        r->out_endp[out_ep_num].doepctl = epctl::DEPCTL_USBACTEP | epctl::DEPCTL_TYPE_BULK
                               | epctl::DEPCTL_SETD0PID
                               | (EP1_MPS & epctl::DEPCTL_MPS_MASK);
        r->daintmsk |= daint::IN_EP(1) | daint::OUT_EP(out_ep_num);
        configured1 = true;
        tx_busy1 = false;
        rx1_pending = false;
        app_console_write("USB1: configured by host\n");
        ep_tx(0, ep0_buf1, 0);        // status ZLP
        ep1_arm_read();               // start listening for keystrokes
        return;
    }

    case 0x08:                        // GET_CONFIGURATION
        ep0_buf1[0] = configured1 ? 1 : 0;
        ep_tx(0, ep0_buf1, wLength < 1 ? wLength : 1);
        return;

    case 0x00:                        // GET_STATUS (device/intf/ep)
        ep0_buf1[0] = 0; ep0_buf1[1] = 0;
        ep_tx(0, ep0_buf1, wLength < 2 ? wLength : 2);
        return;

    default:
        ep0_stall();
        return;
    }
}

} // namespace

// ---- public API ----------------------------------------------------------
extern "C" int usb1i3_is_configured(void) {
    return configured1 ? 1 : 0;
}

/// Snapshot EP1-OUT state for diagnostics.
/// out[]: 0=DOEPCTL1 1=DOEPINT1 2=DOEPTSIZ1 3=DOEPDMA1 4=DAINT 5=DAINTMSK
///        6=rx1_pending 7=GINTSTS
extern "C" void usb1i3_ep1_snapshot(uint32_t out[8]) {
    auto* r = regs1();
    out[0] = r->out_endp[out_ep_num].doepctl;
    out[1] = r->out_endp[out_ep_num].doepint;
    out[2] = r->out_endp[out_ep_num].doeptsiz;
    out[3] = r->out_endp[out_ep_num].doepdma;
    out[4] = r->daint;
    out[5] = r->daintmsk;
    out[6] = rx1_pending ? 1u : 0u;
    out[7] = r->gintsts;
}

/// Snapshot the core's control-path registers for diagnostics.
/// out[]: 0=GINTSTS 1=GINTMSK 2=DAINT 3=DAINTMSK 4=DOEPCTL0 5=DOEPINT0
///        6=DOEPTSIZ0 7=DOEPDMA0 8=DCFG 9=DSTS 10=DCTL 11=GAHBCFG
extern "C" void usb1i3_reg_snapshot(uint32_t out[12]) {
    auto* r = regs1();
    out[0] = r->gintsts;
    out[1] = r->gintmsk;
    out[2] = r->daint;
    out[3] = r->daintmsk;
    out[4] = r->out_endp[0].doepctl;
    out[5] = r->out_endp[0].doepint;
    out[6] = r->out_endp[0].doeptsiz;
    out[7] = r->out_endp[0].doepdma;
    out[8] = r->dcfg;
    out[9] = r->dsts;
    out[10] = r->dctl;
    out[11] = r->gahbcfg;
}

/**
 * @brief Queue one bulk-IN chunk to the host (stage B / future use).
 * @param buf DMA-safe source buffer (must live in .dma_nc).
 * @param len Number of bytes to send.
 * @return 0 on accept, or -1 if the port is not ready/busy.
 */
extern "C" int usb1i3_tx(const void *buf, int len) {
    if (!configured1 || tx_busy1 || len < 0)
        return -1;
    tx_busy1 = true;
    ep_tx(1, buf, static_cast<uint32_t>(len));
    return 0;
}

extern "C" void usb1i3_init(void) {
    // CRITICAL: force the U2501 VBUS load switch OFF before anything else.
    // The PC is the host on the A-to-A cable; the board must never source
    // VBUS back at it.
    constexpr uintptr_t GPIO1_IOC_BASE = 0xFF660000U;
    constexpr uintptr_t GPIO1_BASE     = 0xFF870000U;
    masked_write(GPIO1_IOC_BASE + 0x0038U, 0xF000U, 0x0000U); // D3 -> GPIO func
    masked_write(GPIO1_BASE + 0x000CU, 1U << 11, 1U << 11);   // D3 output
    masked_write(GPIO1_BASE + 0x0004U, 1U << 11, 0x0000U);    // D3 LOW: U2501 off
    dmb();
    mdelay(50);                                               // residual VBUS drain

    // Clocks: CRU_GATE_CON07 bits 8..11 (hclk/pmu/adp/pclk_usbphy) un-gate
    masked_write(CRU1_BASE + 0x081CU, 0x0F00U, 0x0000U);
    dmb();

    // Soft resets: CON07 bits 8,10,11,12,14 and CON09 bit 1 (utmi)
    masked_write(CRU1_BASE + 0x0A1CU, 0x5D00U, 0x5D00U);
    masked_write(CRU1_BASE + 0x0A24U, 0x0002U, 0x0002U);
    dmb();
    mdelay(10);
    masked_write(CRU1_BASE + 0x0A1CU, 0x5D00U, 0x0000U);
    masked_write(CRU1_BASE + 0x0A24U, 0x0002U, 0x0000U);
    dmb();
    mdelay(10);

    // Un-gate the USB PHY 24 MHz reference clock (clk_ref_usbphy_top,
    // GATE_CON01 bit 4) — shared with OTG0; in the stateos bridge OTG0's
    // init always provided it.
    masked_write(CRU1_BASE + 0x0804U, 0x0010U, 0x0000U);

    // Un-gate the DMA-to-DDR bridge (aclk/pclk_dma2ddr, GATE_CON08 bits
    // 0/1). The DWC2 AHB master reaches DDR through this bridge: with it
    // gated, DMA writes are silently dropped (doepdma advances, SUPCNT
    // decrements, but nothing lands in RAM) while register access still
    // works — board-found on OTG0, re-found on OTG1 in smp_test_int3
    // (SETUP buffer stayed 0xFF while the core consumed the packet).
    masked_write(CRU1_BASE + 0x0820U, 0x0003U, 0x0000U);
    dmb();

    // Open the DDR firewall for bus masters: RK3506 boots with FW_DDR_MSTx
    // dropping DMA from unlisted masters (reads as 0, no fault). U-Boot SPL
    // only whitelists sdmmc/fspi — never USB. Same fix as OTG0's driver.
    for (uint32_t off = 0x20U; off <= 0x2CU; off += 4U) {
        *reinterpret_cast<volatile uint32_t*>(0xFF5F0000U + off) = 0x00000000U;
    }
    dmb();

    // GRF_SOC_CON28: mirror OTG0's proven-working PHY port value exactly
    // (see the parent driver for the full war story: bit 11 + iddig force
    // device mode; residual [8:0] forces a FS transceiver; both board-found).
    masked_write(GRF1_BASE + 0x0070U, 0xFFFFU, 0xCE00U);
    dmb();

    auto* r = regs1();

    // Core soft reset
    uint32_t guard = 0;
    while (!(r->grstctl & grstctl::AHB_MASTER_IDLE) && ++guard < 100000) {}
    r->grstctl |= grstctl::CORE_SOFT_RESET;
    guard = 0;
    while ((r->grstctl & grstctl::CORE_SOFT_RESET) && ++guard < 100000) {}
    mdelay(5);

    // Probe the implemented endpoints: unimplemented DAINTMSK bits read
    // back as zero. OTG1's instance may lack an OUT EP1 (EP1 IN-only) —
    // pick the first real OUT endpoint and patch the descriptor to match.
    r->daintmsk = 0xFFFFFFFFU;
    daintmsk_impl = r->daintmsk;
    r->daintmsk = 0;
    for (uint32_t n = 1; n < 16; ++n) {
        if (daintmsk_impl & daint::OUT_EP(n)) {
            out_ep_num = static_cast<uint8_t>(n);
            break;
        }
    }
    cfg_desc1[27] = out_ep_num;       // bulk-OUT bEndpointAddress
    {
        char msg[64];
        char* q = msg;
        const char* s = "USB1: DAINTMSK impl=0x";
        while (*s) *q++ = *s++;
        for (int i = 28; i >= 0; i -= 4)
            *q++ = "0123456789ABCDEF"[(daintmsk_impl >> i) & 0xF];
        s = " -> OUT EP ";
        while (*s) *q++ = *s++;
        *q++ = static_cast<char>('0' + out_ep_num);
        *q++ = '\n';
        *q = 0;
        app_console_write(msg);
    }

    // FIFOs (same partitioning as OTG0)
    r->grxfsiz = 280;
    r->gnptxfsiz = (256U << 16) | 280U;
    r->dieptxf[0] = (256U << 16) | (280U + 256U);

    // AHB DMA, device mode, 16-bit UTMI, HS
    r->gahbcfg = gahbcfg::MODE_DMA | gahbcfg::BURST_INCR4 | gahbcfg::GBL_INT_UNMASK;
    r->gusbcfg = (r->gusbcfg & ~gusbcfg::USBTRDTIM_MASK)
               | gusbcfg::FORCE_DEVICE_MODE
               | gusbcfg::PHYIF_16BIT
               | gusbcfg::USBTRDTIM(5);
    dmb();
    mdelay(30);
#ifdef USB_FORCE_FS
    r->dcfg = (r->dcfg & ~0x3U) | dcfg::DEV_SPEED_FULL_SPEED_20;
#else
    r->dcfg = (r->dcfg & ~0x3U) | dcfg::DEV_SPEED_HIGH_SPEED_20;
#endif

    // These same bits now drive the GIC interrupt line instead of a
    // polling loop (GAHBCFG.GBL_INT_UNMASK asserts the line).
    r->gintsts = 0xFFFFFFFFU;
    r->gintmsk = gintsts::INT_ENUMDONE | gintsts::INT_RESET
               | gintsts::INT_OUT_EP | gintsts::INT_IN_EP;
    r->diepmsk = epint::TRANSFER_DONE | epint::AHB_ERROR;
    r->doepmsk = epint::TRANSFER_DONE | epint::CTRL_OUT_EP_SETUP_PHASE_DONE
               | epint::AHB_ERROR;
    r->daintmsk = daint::IN_EP(0) | daint::OUT_EP(0);
    dmb();

    ep0_arm_setup();

    // Connect (clear soft disconnect)
    r->dctl &= ~dctl::SOFT_DISCONNECT;
    dmb();
    app_console_write("USB1: OTG1 device-mode init done (2207:001B)\n");
}

/**
 * @brief Service the OTG1 core — called from the GIC IRQ path
 *        (port_external_irq_handler). Same event handling as the parent's
 *        usb1_poll(), minus the sink machinery, plus auto re-armed reads.
 */
extern "C" void usb1i3_service(void) {
    auto* r = regs1();
    uint32_t sts = r->gintsts & r->gintmsk;
    usb1i3_dbg[0] = usb1i3_dbg[0] + 1;
    usb1i3_dbg[7] = sts;
    if (!sts)
        return;

    if (sts & gintsts::INT_RESET) {
        usb1i3_dbg[1] = usb1i3_dbg[1] + 1;
        r->gintsts = gintsts::INT_RESET;
        r->dcfg &= ~dcfg::DCFG_DEVADDR_MASK;
        configured1 = false;
        tx_busy1 = false;
        rx1_pending = false;
        for (int i = 0; i < 16; ++i)
            r->out_endp[i].doepctl |= epctl::DEPCTL_SNAK;
        r->daintmsk = daint::IN_EP(0) | daint::OUT_EP(0);
        app_console_write("USB1: bus reset\n");
        ep0_arm_setup();
    }

    if (sts & gintsts::INT_ENUMDONE) {
        usb1i3_dbg[2] = usb1i3_dbg[2] + 1;
        r->gintsts = gintsts::INT_ENUMDONE;
        r->in_endp[0].diepctl = (r->in_endp[0].diepctl & ~epctl::DEPCTL_MPS_MASK)
                              | epctl::DEPCTL0_MPS_64 | epctl::DEPCTL_USBACTEP;
        r->out_endp[0].doepctl |= epctl::DEPCTL_USBACTEP | epctl::DEPCTL_CNAK;
        dmb();
        // Re-arm SETUP reception for the new bus state: the reset that
        // preceded this ENUMDONE may have consumed the previous arming
        ep0_arm_setup();
        app_console_write((r->dsts & 0x6U) == 0 ? "USB1: HIGH-SPEED\n"
                                                : "USB1: FULL-SPEED\n");
    }

    if (sts & gintsts::INT_OUT_EP) {
        usb1i3_dbg[3] = usb1i3_dbg[3] + 1;
        uint32_t ep_intr = (r->daint & r->daintmsk) >> 16;
        if (ep_intr & 1U) {
            uint32_t st = r->out_endp[0].doepint;
            r->out_endp[0].doepint = st;
            if (st & epint::CTRL_OUT_EP_SETUP_PHASE_DONE)
              {
                usb1i3_dbg[4] = usb1i3_dbg[4] + 1;
                handle_setup1();
              }
            else if (st & epint::TRANSFER_DONE)
                ep0_arm_setup();
        }
        if (ep_intr & (1U << out_ep_num)) {
            uint32_t st = r->out_endp[out_ep_num].doepint;
            r->out_endp[out_ep_num].doepint = st;
            if ((st & epint::TRANSFER_DONE) && rx1_pending) {
                usb1i3_dbg[6] = usb1i3_dbg[6] + 1;
                // received = armed - remaining (XFERSIZE counts down)
                uint32_t remaining =
                    r->out_endp[out_ep_num].doeptsiz & epsiz::DOEPT_SIZ_XFER_SIZE_MAX_EP;
                uint32_t got = rx1_armed - remaining;
                rx1_pending = false;
                invalidate_dcache_range(
                    reinterpret_cast<uint32_t>(cmd1_buf),
                    reinterpret_cast<uint32_t>(cmd1_buf) + sizeof cmd1_buf);
                if (got > 0)
                    usb1i3_rx_packet(cmd1_buf, got);
                if (configured1)
                    ep1_arm_read();   // immediately listen for the next one
            }
        }
    }

    if (sts & gintsts::INT_IN_EP) {
        usb1i3_dbg[5] = usb1i3_dbg[5] + 1;
        uint32_t ep_intr = r->daint & r->daintmsk & 0xFFFFU;
        if (ep_intr & 1U) {
            uint32_t st = r->in_endp[0].diepint;
            r->in_endp[0].diepint = st;
            if (st & epint::TRANSFER_DONE)
                ep0_arm_setup();        // control transaction done, next SETUP
        }
        if (ep_intr & 2U) {
            uint32_t st = r->in_endp[1].diepint;
            r->in_endp[1].diepint = st;
            if ((st & epint::TRANSFER_DONE) && tx_busy1) {
                tx_busy1 = false;
                usb1i3_tx_done();
            }
        }
    }
}
