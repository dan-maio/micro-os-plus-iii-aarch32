/**
 * @file dwc2_ep0.cpp
 * @brief Endpoint 0 (Control) setup packet handling and state machine implementation for DWC2.
 */

#include "dwc2_driver.hpp"
#include "dwc2_gadget.hpp"

extern "C" {
    void invalidate_dcache_range(uint32_t start, uint32_t stop);
    void uart_puts(const char* s);
    char *hex32(uint32_t val);
}

namespace rk3506::usb::dwc2 {

// Reference to global instances defined in dwc2_gadget.cpp
extern struct usb_gadget dwc2_gadget_instance;

void Driver::handle_ep0_setup() noexcept {
    // 1. Invalidate CPU cache to read the DMA-filled setup buffer
    invalidate_dcache_range(reinterpret_cast<uint32_t>(setup_buffer_),
                            reinterpret_cast<uint32_t>(setup_buffer_) + 64);

    // 2. Locate the packet. With SUPCNT armed, the core auto-increments
    // doepdma by 8 for every SETUP received, so back-to-back SETUPs land at
    // offsets 8, 16, ... The freshest packet sits just below the current
    // doepdma readback; offset 0 would be a stale earlier packet.
    uint32_t base = reinterpret_cast<uint32_t>(setup_buffer_);
    uint32_t dma_now = regs_->out_endp[0].doepdma;
    const uint8_t* pkt = setup_buffer_;
    if (dma_now >= base + 8U && dma_now <= base + sizeof(setup_buffer_)) {
        pkt = reinterpret_cast<const uint8_t*>(dma_now - 8U);
    }
    auto* ctrl = reinterpret_cast<const struct usb_ctrlrequest*>(pkt);

    // Latch the direction of whatever the gadget driver queues next on EP0.
    // With a data stage it follows bmRequestType bit 7; a no-data request
    // (wLength == 0) skips straight to the status stage, which is always IN,
    // so the composite driver's zero-length status queue must be a TX.
    ep0_next_in_ = (ctrl->wLength == 0) || ((ctrl->bRequestType & 0x80U) != 0);

    // 3. Log raw setup bytes (2 words) for bring-up debugging
    uart_puts("DWC2 Setup: ");
    uart_puts(hex32(reinterpret_cast<const uint32_t*>(pkt)[0]));
    uart_puts(" ");
    uart_puts(hex32(reinterpret_cast<const uint32_t*>(pkt)[1]));
    uart_puts(" [dma=");
    uart_puts(hex32(dma_now));          // buffer base was programmed; did it advance?
    uart_puts(" tsiz=");
    uart_puts(hex32(regs_->out_endp[0].doeptsiz)); // did SUPCNT/XFERSIZE decrement?
    uart_puts(" ahb=");
    uart_puts(hex32(regs_->gahbcfg));   // is DMA mode actually on?
    uart_puts(" ctl=");
    uart_puts(hex32(regs_->out_endp[0].doepctl));
    uart_puts("]\n");

    bool req_std = (ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD;
    int ret = -1;

    // 3. Handle standard requests locally if they modify low-level registers
    if (req_std) {
        switch (ctrl->bRequest) {
        case USB_REQ_SET_ADDRESS:
            connected_ = true;
            // Write address to DCFG. Controller automatically delays application
            // until the status stage of this control transfer completes.
            set_address(ctrl->wValue & 0x7FU);
            
            // Send Zero-Length Status Packet (ZLP) on EP0-IN
            start_tx(0, setup_buffer_, 0, 0);
            return;

        case USB_REQ_SET_CONFIGURATION:
            state_ = rk3506::usb::State::configured;
            // Config setup will also be routed to composite driver below
            break;

        default:
            break;
        }
    }

    // 4. Route setup packet to the registered composite driver
    if (active_gadget_driver && active_gadget_driver->setup) {
        ret = active_gadget_driver->setup(&dwc2_gadget_instance, ctrl);
    }


    // 5. Manage status or stall phases based on driver return code
    if (ret < 0) {
        uart_puts("DWC2 Setup: Unhandled request. Stalling EP0.\n");
        // Protocol Stall EP0 IN & OUT
        stall(0, true, true);
        stall(0, false, true);
        
        // Re-arm EP0-OUT for next Setup packet
        start_rx(0, setup_buffer_, 8);
    }
    // On success no local action: composite_setup() queues the EP0 response
    // itself for every request it handles -- data stage or zero-length
    // status alike (usb_ep_queue at its tail). Queueing a second ZLP here
    // double-arms EP0-IN and corrupts the control state machine.
}

} // namespace rk3506::usb::dwc2
