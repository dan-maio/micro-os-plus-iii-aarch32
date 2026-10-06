#include "dwc2_gadget.hpp"
#include "dwc2_driver.hpp"

extern "C" {
    void uart_puts(const char* s);
    char *hex32(uint32_t val);
}

namespace rk3506::usb::dwc2 {

// Defined below (global UDC section); needed by queue() for EP0 detection
extern struct usb_gadget dwc2_gadget_instance;

// Static definition of standard endpoint operations
static const struct usb_ep_ops dwc2_ep_ops = {
    .enable        = ep_enable,
    .disable       = ep_disable,
    .alloc_request = alloc_request,
    .free_request  = free_request,
    .queue         = queue,
    .dequeue       = dequeue,
    .set_halt      = set_halt,
    .set_wedge     = set_wedge,
    .fifo_status   = fifo_status,
    .fifo_flush    = fifo_flush,
};

int ep_enable(struct usb_ep *ep, const struct usb_endpoint_descriptor *desc) noexcept {
    if (!ep || !desc) return -EINVAL;

    ep->desc = desc;
    ep->maxpacket = le16_to_cpu(desc->wMaxPacketSize);
    ep->ops = &dwc2_ep_ops;

    uint8_t ep_num = desc->bEndpointAddress & 0x0FU;
    bool is_in = (desc->bEndpointAddress & USB_DIR_IN) != 0;

    auto* regs = get_regs();
    
    // Clear Stall and enable endpoint in registers
    if (is_in) {
        uint32_t ctrl = regs->in_endp[ep_num].diepctl;
        ctrl &= ~epctl::DEPCTL_STALL;
        ctrl &= ~epctl::DEPCTL_MPS_MASK;
        ctrl |= ep->maxpacket & epctl::DEPCTL_MPS_MASK; // MPS=0 is invalid for EPn
        ctrl |= epctl::DEPCTL_USBACTEP;

        // Match transfer type
        uint8_t type = desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
        if (type == USB_ENDPOINT_XFER_BULK) {
            ctrl |= epctl::DEPCTL_TYPE_BULK;
        } else if (type == USB_ENDPOINT_XFER_INT) {
            ctrl |= epctl::DEPCTL_TYPE_INTR;
        }

        // Map Tx FIFO number (Bulk EP1-IN uses FIFO 1, EP2-IN uses FIFO 2, etc.)
        ctrl &= epctl::DIEPCTL_TX_FIFO_NUM_MASK;
        ctrl |= epctl::DIEPCTL_TX_FIFO_NUM(ep_num);

        // Start with DATA0 PID
        ctrl |= epctl::DEPCTL_SETD0PID;

        regs->in_endp[ep_num].diepctl = ctrl;
    } else {
        uint32_t ctrl = regs->out_endp[ep_num].doepctl;
        ctrl &= ~epctl::DEPCTL_STALL;
        ctrl &= ~epctl::DEPCTL_MPS_MASK;
        ctrl |= ep->maxpacket & epctl::DEPCTL_MPS_MASK;
        ctrl |= epctl::DEPCTL_USBACTEP;

        uint8_t type = desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
        if (type == USB_ENDPOINT_XFER_BULK) {
            ctrl |= epctl::DEPCTL_TYPE_BULK;
        } else if (type == USB_ENDPOINT_XFER_INT) {
            ctrl |= epctl::DEPCTL_TYPE_INTR;
        }

        // Start with DATA0 PID
        ctrl |= epctl::DEPCTL_SETD0PID;

        regs->out_endp[ep_num].doepctl = ctrl;
    }

    // Route this endpoint's interrupts into daint
    regs->daintmsk |= is_in ? daint::IN_EP(ep_num) : daint::OUT_EP(ep_num);

    uart_puts("DWC2: Enabled Endpoint ");
    uart_puts(ep->name);
    uart_puts("\n");

    return 0;
}

int ep_disable(struct usb_ep *ep) noexcept {
    if (!ep || !ep->desc) return -EINVAL;

    uint8_t ep_num = ep->desc->bEndpointAddress & 0x0FU;
    bool is_in = (ep->desc->bEndpointAddress & USB_DIR_IN) != 0;

    auto* regs = get_regs();

    if (is_in) {
        // Stop IN endpoint: set NAK, disable endpoint
        regs->in_endp[ep_num].diepctl |= epctl::DEPCTL_SNAK;
        regs->in_endp[ep_num].diepctl &= ~epctl::DEPCTL_EPENA;
    } else {
        // Stop OUT endpoint: set NAK, disable endpoint
        regs->out_endp[ep_num].doepctl |= epctl::DEPCTL_SNAK;
        regs->out_endp[ep_num].doepctl &= ~epctl::DEPCTL_EPENA;
    }

    ep->desc = nullptr;

    uart_puts("DWC2: Disabled Endpoint ");
    uart_puts(ep->name);
    uart_puts("\n");

    return 0;
}

struct usb_request *alloc_request(struct usb_ep * /*ep*/, unsigned /*gfp_flags*/) noexcept {
    auto* req = static_cast<struct usb_request*>(malloc(sizeof(struct usb_request)));
    if (!req) return nullptr;

    memset(req, 0, sizeof(struct usb_request));
    req->list.next = &req->list;
    req->list.prev = &req->list;
    return req;
}

void free_request(struct usb_ep * /*ep*/, struct usb_request *req) noexcept {
    if (req) {
        free(req);
    }
}

int queue(struct usb_ep *ep, struct usb_request *req, unsigned /*gfp_flags*/) noexcept {
    if (!ep || !req) return -EINVAL;

    uint8_t ep_num;
    bool is_in;
    if (ep->desc) {
        ep_num = ep->desc->bEndpointAddress & 0x0FU;
        is_in = (ep->desc->bEndpointAddress & USB_DIR_IN) != 0;
    } else if (ep == dwc2_gadget_instance.ep0) {
        // EP0 has no descriptor; the data stage direction follows the
        // bmRequestType bit of the current SETUP packet.
        ep_num = 0;
        is_in = Driver::instance().ep0_data_in();
    } else {
        return -EINVAL;
    }

    req->status = -115; // -EINPROGRESS (standard U-Boot code)
    req->actual = 0;

    // Record the active request so the interrupt handler can run its
    // completion callback, then trigger DMA immediately (queue depth 1)
    Driver::instance().set_pending_request(ep_num, is_in, ep, req);
    if (is_in) {
        // Logical EPn-IN maps to Tx FIFO n
        Driver::instance().start_tx(ep_num, req->buf, req->length, ep_num);
    } else {
        Driver::instance().start_rx(ep_num, req->buf, req->length);
    }

    return 0;
}

int dequeue(struct usb_ep *ep, struct usb_request *req) noexcept {
    if (!req) return -EINVAL;

    // Drop the pending slot so a late TRANSFER_DONE cannot complete a
    // cancelled request
    if (ep && ep->desc) {
        uint8_t ep_num = ep->desc->bEndpointAddress & 0x0FU;
        bool is_in = (ep->desc->bEndpointAddress & USB_DIR_IN) != 0;
        Driver::instance().set_pending_request(ep_num, is_in, nullptr, nullptr);
    }

    list_del(&req->list);
    req->status = -104; // -ECONNRESET
    return 0;
}

int set_halt(struct usb_ep *ep, int value) noexcept {
    if (!ep || !ep->desc) return -EINVAL;

    uint8_t ep_num = ep->desc->bEndpointAddress & 0x0FU;
    bool is_in = (ep->desc->bEndpointAddress & USB_DIR_IN) != 0;

    Driver::instance().stall(ep_num, is_in, value != 0);
    return 0;
}

int set_wedge(struct usb_ep * /*ep*/) noexcept {
    return -1; // Not supported on bare-metal
}

int fifo_status(struct usb_ep * /*ep*/) noexcept {
    return 0; // Not monitored on DMA mode
}

void fifo_flush(struct usb_ep *ep) noexcept {
    if (!ep || !ep->desc) return;

    uint8_t ep_num = ep->desc->bEndpointAddress & 0x0FU;
    bool is_in = (ep->desc->bEndpointAddress & USB_DIR_IN) != 0;

    if (is_in) {
        auto* regs = get_regs();
        regs->grstctl = grstctl::TX_FIFO_NUMBER(ep_num);
        regs->grstctl |= grstctl::TX_FIFO_FLUSH;
        while (regs->grstctl & grstctl::TX_FIFO_FLUSH) {
            __asm__ volatile("nop");
        }
    }
}

// -------------------------------------------------------------------------
// Global UDC start/stop implementations
// -------------------------------------------------------------------------

static struct usb_ep dwc2_ep_array[8];
struct usb_gadget dwc2_gadget_instance;

static int dwc2_pullup(struct usb_gadget * /*g*/, int is_on) {
    auto* regs = get_regs();
    if (is_on) {
        regs->dctl &= ~dctl::SOFT_DISCONNECT; // Connect
    } else {
        regs->dctl |= dctl::SOFT_DISCONNECT;  // Disconnect
    }
    return 0;
}

static const struct usb_gadget_ops dwc2_gadget_ops = {
    .pullup    = dwc2_pullup,
    .udc_start = udc_start,   // called via ops->udc_start (no null check upstream)
    .udc_stop  = udc_stop,
};

// Endpoint names follow the U-Boot epautoconf convention so ep_matches()
// can pair them with descriptors: digit = fixed hw endpoint number,
// "in"/"out" = direction, "-bulk" = type.
// The set must mirror GHWCFG1 (0x00006664 on RK3506 OTG0): each EPn is
// synthesized single-direction, alternating IN/OUT -- there is no ep1out
// in silicon (its DAINTMSK bit doesn't even latch), so bulk OUT lives on
// EP2, the next IN on EP3, and so on.
static const char* const dwc2_ep_names[8] = {
    "ep0",
    "ep1in-bulk", "ep2out-bulk",
    "ep3in-bulk", "ep4out-bulk",
    "ep5in-bulk", "ep6out-bulk",
    "ep7in-bulk",
};

extern "C" int dwc2_gadget_probe() {
    struct usb_gadget *g = &dwc2_gadget_instance;
    g->name = "dwc2-udc";
    g->ops = &dwc2_gadget_ops;
    g->speed = USB_SPEED_UNKNOWN;
    g->max_speed = USB_SPEED_HIGH;
    g->is_dualspeed = 1;
    g->ep0 = &dwc2_ep_array[0];
    g->ep0->name = dwc2_ep_names[0];
    g->ep0->ops = &dwc2_ep_ops;
    g->ep0->maxpacket = 64;
    g->ep0->maxpacket_limit = 64;

    // Register the logical endpoints on the gadget's endpoint list.
    // ep0 stays off the list (control EPs are excluded from autoconfig),
    // matching the U-Boot dwc2_udc_otg convention.
    INIT_LIST_HEAD(&g->ep_list);
    for (uint8_t i = 1; i < 8; ++i) {
        dwc2_ep_array[i].name = dwc2_ep_names[i];
        dwc2_ep_array[i].ops = &dwc2_ep_ops;
        dwc2_ep_array[i].desc = nullptr;
        dwc2_ep_array[i].driver_data = nullptr;
        dwc2_ep_array[i].maxpacket = 512;       // HS bulk
        dwc2_ep_array[i].maxpacket_limit = 512;
        list_add_tail(&dwc2_ep_array[i].ep_list, &g->ep_list);
    }

    // Add to UDC list
    return usb_add_gadget_udc(nullptr, g);
}


int udc_start(struct usb_gadget * /*g*/, struct usb_gadget_driver *driver) noexcept {
    // Initialize Low-Level DWC2 Core
    if (Driver::instance().initialize() != DriverStatus::OK) {
        return -ENODEV;
    }

    active_gadget_driver = driver;
    Driver::instance().set_state(rk3506::usb::State::powered);
    return 0;
}

int udc_stop(struct usb_gadget * /*g*/) noexcept {
    auto* regs = get_regs();
    regs->dctl |= dctl::SOFT_DISCONNECT; // Terminate connection
    regs->gintmsk = 0; // Disable interrupts
    active_gadget_driver = nullptr;
    Driver::instance().set_state(rk3506::usb::State::disconnected);
    return 0;
}

struct usb_gadget_driver* active_gadget_driver = nullptr;

} // namespace rk3506::usb::dwc2



