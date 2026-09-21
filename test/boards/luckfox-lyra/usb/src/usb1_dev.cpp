/**
 * @file usb1_dev.cpp
 * @brief Minimal USB OTG1 (0xFF780000) DEVICE-mode gadget: PC1's port.
 *
 * OTG1 carries the Lyra's Type-A connector, normally a host port; here the
 * GRF UTMI overrides force DEVICE mode (iddig=1) so PC1 can enumerate it
 * over an A-to-A cable. The VBUS load switch (GPIO1_D3 / U2501) is left
 * OFF: PC1 is the host and must not see the board driving 5V back.
 *
 * Deliberately independent of the composite/UDC framework (which is
 * single-instance): a hardcoded descriptor set (2207:001A, one interface,
 * bulk IN 0x81 + bulk OUT 0x01), a polled EP0 state machine, a one-shot
 * bulk TX API and — since the two-PC symmetry work — an RX side that
 * mirrors the OTG0 vendor gadget's semantics:
 *
 *   - usb1_start_read_queue(): arm ONE 512-byte command read (self-guarded:
 *     never while a read is pending or a sink owns the endpoint — the same
 *     stall-window rule that was board-found on OTG0);
 *   - completed command packets go to the weak usb1_command() hook (the
 *     usb1 twin of station_command);
 *   - usb1_sink_start/ring_avail/ring_read/sink_finished/sink_end: a
 *     streaming sink into an internal .dma_nc ring, arms capped at the
 *     bytes still expected ROUNDED UP TO 512 (a mid-packet arm hangs the
 *     controller; an over-arm past a multiple-of-MPS stream never
 *     completes — both board-found on OTG0 and inherited here).
 *
 * TX completions call back into bridge_usb1_tx_done (the generic
 * "usb1 IN finished" hook, despite the historic name).
 */

#include <stdint.h>
#include "dwc2_regs.hpp"

/* Environment services provided by usb_env_stateos.cpp (see that file). */
extern "C" {
    void uart_puts(const char *s);
    /// Format a word as 8 hex digits. @param val Value. @return pointer to a static buffer.
    char *hex32(uint32_t val);
    /// D-cache invalidate hook (no-op with .dma_nc). @param start Range begin. @param stop Range end.
    void invalidate_dcache_range(uint32_t start, uint32_t stop);
    /// D-cache clean hook (no-op with .dma_nc). @param start Range begin. @param stop Range end.
    void flush_dcache_range(uint32_t start, uint32_t stop);
    void mdelay(unsigned long ms);
    void udelay(unsigned long us);

    // Implemented by the bridge glue in usb_vendor_gadget.cpp
    void bridge_usb1_tx_done(void);
    void bridge_pump(void);
}

/// Command hook for packets received on the USB1 OUT endpoint outside a sink
/// (the usb1 twin of station_command). Weak: images without a usb1-ingress
/// route simply never see commands. @return nonzero if consumed.
extern "C" int usb1_command(const uint8_t* pkt, uint32_t len) __attribute__((weak));

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
// setup_buf1 (EP0 SETUP DMA target) and ep0_buf1 (EP0 IN DMA source) are
// touched by the DWC2 DMA engine. Under StateOS the cache-range hooks are
// no-ops (coherency comes from the Normal-NC window), so these MUST sit in
// .dma_nc — a NOLOAD section, fine for these uninitialized buffers. The
// descriptors below are only ever CPU-copied into ep0_buf1, never DMA'd, so
// they stay in normal (initialized) memory.
uint8_t setup_buf1[64] __attribute__((aligned(64), section(".dma_nc")));
uint8_t ep0_buf1[64]   __attribute__((aligned(64), section(".dma_nc")));

bool     configured1   = false;
bool     tx_busy1      = false;
uint8_t  address1      = 0;

// ---- RX side (bulk OUT 0x01): command reads + streaming sink -----------
// All of this is touched ONLY from the one task that calls usb1_poll() and
// the usb1_* APIs (single-owner invariant, as on OTG0) — no locking needed.
#ifdef USB_FORCE_FS
// FS MPS=64: DOEPTSIZ PKTCNT is 10 bits, so cap the arm at 512 packets.
constexpr uint32_t RX1_CHUNK = 32768;
#else
constexpr uint32_t RX1_CHUNK = 65536;
#endif
constexpr uint32_t RX1_RING_SIZE = 2 * RX1_CHUNK * (65536 / RX1_CHUNK); // 128 KB
uint8_t rx1_ring[RX1_RING_SIZE] __attribute__((aligned(64), section(".dma_nc")));
uint8_t cmd1_buf[512]           __attribute__((aligned(64), section(".dma_nc")));

bool     rx1_pending  = false;   ///< an OUT transfer is armed on EP1
bool     rx1_is_cmd   = false;   ///< the armed OUT is a 512-byte command read
uint32_t rx1_armed    = 0;       ///< bytes armed for the in-flight OUT
int      sink1_active = 0;       ///< a streaming sink session is open
uint32_t sink1_total  = 0;       ///< bytes the sender declared
uint32_t sink1_rx     = 0;       ///< bytes landed in the ring so far
uint32_t rx1_head = 0, rx1_tail = 0, rx1_count = 0;
int      rx1_stalled  = 0;       ///< ring was full; drain re-arms

// ---- descriptors (hardcoded; no strings so indexes stay 0) ------------
alignas(64) const uint8_t dev_desc1[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    0x07, 0x22,             // idVendor  0x2207 (Rockchip)
    0x1A, 0x00,             // idProduct 0x001A (bridge output port)
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
alignas(64) const uint8_t cfg_desc1[32] = {
    9, 0x02, 32, 0, 1, 1, 0, 0xC0, 50,
    9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    7, 0x05, 0x81, 0x02,
    static_cast<uint8_t>(EP1_MPS & 0xFF),
    static_cast<uint8_t>(EP1_MPS >> 8), 0x00, // EP1-IN bulk
    7, 0x05, 0x01, 0x02,
    static_cast<uint8_t>(EP1_MPS & 0xFF),
    static_cast<uint8_t>(EP1_MPS >> 8), 0x00, // EP1-OUT bulk (symmetry RX)
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
    auto* r = regs1();
    r->in_endp[0].diepctl  |= epctl::DEPCTL_STALL;
    r->out_endp[0].doepctl |= epctl::DEPCTL_STALL;
    ep0_arm_setup();
}

// ---- EP1-OUT (RX) helpers ----------------------------------------------

/// Arm one bulk-OUT transfer on EP1. @param buf .dma_nc destination.
/// @param len Bytes to arm — MUST be a 512-multiple (an arm ending
/// mid-packet hangs the controller; board-found on OTG0).
void ep1_arm_out(uint8_t* buf, uint32_t len) {
    auto* r = regs1();
    invalidate_dcache_range(reinterpret_cast<uint32_t>(buf),
                            reinterpret_cast<uint32_t>(buf) + len);
    r->out_endp[1].doepdma  = reinterpret_cast<uint32_t>(buf);
    r->out_endp[1].doeptsiz = epsiz::DIEPT_SIZ_PKT_CNT(len / EP1_MPS)
                            | epsiz::DIEPT_SIZ_XFER_SIZE(len);
    r->out_endp[1].doepctl |= epctl::DEPCTL_EPENA | epctl::DEPCTL_CNAK;
    rx1_pending = true;
    rx1_armed   = len;
    dmb();
}

/// Arm the next sink chunk: contiguous in the ring, capped at the bytes
/// still expected rounded UP to 512 (the OTG0 big_arm_rx rules verbatim).
void sink1_arm_rx() {
    uint32_t len = RX1_CHUNK;
    if (len > RX1_RING_SIZE - rx1_head)
        len = RX1_RING_SIZE - rx1_head;
    uint32_t remaining = sink1_total - sink1_rx;
    uint32_t rem_aligned = (remaining + 511u) & ~511u;
    if (len > rem_aligned)
        len = rem_aligned;
    if (len == 0)
        return;
    if (RX1_RING_SIZE - rx1_count < len) {  // no room: drain re-arms
        rx1_stalled = 1;
        return;
    }
    rx1_is_cmd = false;
    ep1_arm_out(&rx1_ring[rx1_head], len);
}

/// Reset every RX/sink bookkeeping field (bus reset / sink end).
void rx1_reset() {
    rx1_pending = false; rx1_is_cmd = false; rx1_armed = 0;
    sink1_active = 0; sink1_total = 0; sink1_rx = 0;
    rx1_head = rx1_tail = rx1_count = 0; rx1_stalled = 0;
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
        r->out_endp[1].doepctl = epctl::DEPCTL_USBACTEP | epctl::DEPCTL_TYPE_BULK
                               | epctl::DEPCTL_SETD0PID
                               | (EP1_MPS & epctl::DEPCTL_MPS_MASK);
        r->daintmsk |= daint::IN_EP(1) | daint::OUT_EP(1);
        configured1 = true;
        tx_busy1 = false;
        rx1_reset();
        uart_puts("USB1: configured by PC1 host\n");
        ep_tx(0, ep0_buf1, 0);        // status ZLP
        bridge_pump();                // a waiting bridge transfer may resume
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
extern "C" int usb1_is_configured(void) {
    return configured1 ? 1 : 0;
}

extern "C" int usb1_tx_busy(void) {
    return tx_busy1 ? 1 : 0;
}

/**
 * @brief Queue one bulk-IN chunk to PC1.
 * @param buf DMA-safe source buffer (must live in .dma_nc).
 * @param len Number of bytes to send.
 * @return 0 on accept, or -1 if the port is not ready/busy.
 */
extern "C" int usb1_tx(const void *buf, int len) {
    if (!configured1 || tx_busy1 || len < 0)
        return -1;
    tx_busy1 = true;
    ep_tx(1, buf, static_cast<uint32_t>(len));
    return 0;
}

/**
 * @brief Arm ONE 512-byte command read on the USB1 OUT endpoint.
 *
 * Self-guarded like OTG0's usb_vendor_start_read_queue: a no-op while not
 * configured, while any OUT is already in flight, or while a sink session
 * owns the endpoint (arming a command read mid-sink swallows payload —
 * the board-found OTG0 stall-window bug class). Call it every owner-loop
 * iteration; completed packets are delivered to the weak usb1_command().
 */
extern "C" void usb1_start_read_queue(void) {
    if (!configured1 || rx1_pending || sink1_active)
        return;
    rx1_is_cmd = true;
    ep1_arm_out(cmd1_buf, sizeof cmd1_buf);
}

/**
 * @brief Open a streaming sink session: @p total bytes will be received
 *        into the internal ring (the usb1 twin of station_start_sink).
 */
extern "C" void usb1_sink_start(uint32_t total) {
    rx1_pending = false;             // a pending command read is abandoned
    rx1_is_cmd  = false;
    sink1_active = 1;
    sink1_total  = total;
    sink1_rx     = 0;
    rx1_head = rx1_tail = rx1_count = 0;
    rx1_stalled = 0;
    if (total > 0 && configured1)
        sink1_arm_rx();
}

extern "C" uint32_t usb1_ring_avail(void) { return rx1_count; }

/// Drain up to @p max bytes from the sink ring (resumes a stalled RX).
/// @param dst Destination. @param max Max bytes. @return bytes copied.
extern "C" uint32_t usb1_ring_read(void* dst, uint32_t max) {
    uint32_t n = rx1_count;
    if (n > max) n = max;
    if (n == 0) return 0;

    uint32_t first = RX1_RING_SIZE - rx1_tail;
    if (first > n) first = n;
    for (uint32_t i = 0; i < first; ++i)
        static_cast<uint8_t*>(dst)[i] = rx1_ring[rx1_tail + i];
    for (uint32_t i = first; i < n; ++i)
        static_cast<uint8_t*>(dst)[i] = rx1_ring[i - first];

    rx1_tail   = (rx1_tail + n) % RX1_RING_SIZE;
    rx1_count -= n;

    if (rx1_stalled && !rx1_pending && sink1_active &&
        RX1_RING_SIZE - rx1_count >= RX1_CHUNK) {
        rx1_stalled = 0;
        sink1_arm_rx();
    }
    return n;
}

/// @return non-zero once ALL expected bytes were received AND drained.
extern "C" int usb1_sink_finished(void) {
    return sink1_active && sink1_rx >= sink1_total && rx1_count == 0;
}

/// Close the sink session (command reads may be armed again afterwards).
extern "C" void usb1_sink_end(void) { rx1_reset(); }

extern "C" void usb1_device_init(void) {
    // CRITICAL: force the U2501 VBUS load switch OFF before anything else.
    // PC1 drives 5V on the A-to-A cable; the board must never source VBUS
    // back at it. Driving GPIO1_D3 LOW explicitly is deterministic
    // regardless of what a previous boot stage left on the pin.
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

    // GRF_SOC_CON28: mirror OTG0's proven-working PHY port value exactly.
    // GRF dump with OTG0 running at HS: [0x60] = 0xCE00 (bvalid force
    // [15:14], bit 11 set, iddig=1 [10], iddig_en [9], force-field
    // [8:0] = 0). OTG1's reset state lacked bit 11 and carried 0x52 in
    // [8:0] — with the enable bit that residual field forces a FS
    // transceiver mode (hence FULL-SPEED-only), and zeroing [8:0]
    // without bit 11 killed the UTMI clock entirely (AHB hang / no
    // enumeration in earlier experiments).
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

    // Interrupt status is consumed by polling; unmask what we inspect
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
    uart_puts("USB1: OTG1 device-mode init done (2207:001A, EP1-IN bulk)\n");
}

/** Polled service loop for OTG1 (no GIC routing for the second core). */
extern "C" void usb1_poll(void) {
    auto* r = regs1();
    uint32_t sts = r->gintsts & r->gintmsk;
    if (!sts)
        return;

    if (sts & gintsts::INT_RESET) {
        r->gintsts = gintsts::INT_RESET;
        r->dcfg &= ~dcfg::DCFG_DEVADDR_MASK;
        configured1 = false;
        tx_busy1 = false;
        rx1_reset();
        for (int i = 0; i < 16; ++i)
            r->out_endp[i].doepctl |= epctl::DEPCTL_SNAK;
        r->daintmsk = daint::IN_EP(0) | daint::OUT_EP(0);
        uart_puts("USB1: bus reset\n");
        ep0_arm_setup();
    }

    if (sts & gintsts::INT_ENUMDONE) {
        r->gintsts = gintsts::INT_ENUMDONE;
        r->in_endp[0].diepctl = (r->in_endp[0].diepctl & ~epctl::DEPCTL_MPS_MASK)
                              | epctl::DEPCTL0_MPS_64 | epctl::DEPCTL_USBACTEP;
        r->out_endp[0].doepctl |= epctl::DEPCTL_USBACTEP | epctl::DEPCTL_CNAK;
        dmb();
        // Re-arm SETUP reception for the new bus state: the reset that
        // preceded this ENUMDONE may have consumed the previous arming
        ep0_arm_setup();
        uart_puts((r->dsts & 0x6U) == 0 ? "USB1: HIGH-SPEED\n" : "USB1: FULL-SPEED\n");
    }

    if (sts & gintsts::INT_OUT_EP) {
        uint32_t ep_intr = (r->daint & r->daintmsk) >> 16;
        if (ep_intr & 1U) {
            uint32_t st = r->out_endp[0].doepint;
            r->out_endp[0].doepint = st;
            if (st & epint::CTRL_OUT_EP_SETUP_PHASE_DONE)
                handle_setup1();
            else if (st & epint::TRANSFER_DONE)
                ep0_arm_setup();
        }
        if (ep_intr & 2U) {
            uint32_t st = r->out_endp[1].doepint;
            r->out_endp[1].doepint = st;
            if ((st & epint::TRANSFER_DONE) && rx1_pending) {
                // received = armed - remaining (XFERSIZE counts down)
                uint32_t remaining =
                    r->out_endp[1].doeptsiz & epsiz::DOEPT_SIZ_XFER_SIZE_MAX_EP;
                uint32_t got = rx1_armed - remaining;
                rx1_pending = false;
                if (rx1_is_cmd) {
                    rx1_is_cmd = false;
                    invalidate_dcache_range(
                        reinterpret_cast<uint32_t>(cmd1_buf),
                        reinterpret_cast<uint32_t>(cmd1_buf) + sizeof cmd1_buf);
                    if (got > 0 && usb1_command)
                        usb1_command(cmd1_buf, got);
                    // no auto re-arm: the owner loop's usb1_start_read_queue
                    // re-arms — unless the command opened a sink, which owns
                    // the endpoint now (the OTG0 stall-window rule).
                } else if (sink1_active) {
                    rx1_head   = (rx1_head + got) % RX1_RING_SIZE;
                    rx1_count += got;
                    sink1_rx  += got;
                    if (sink1_rx < sink1_total)
                        sink1_arm_rx();          // stalls itself if ring full
                }
            }
        }
    }

    if (sts & gintsts::INT_IN_EP) {
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
                bridge_usb1_tx_done();  // advance the bridge state machine
            }
        }
    }
}
