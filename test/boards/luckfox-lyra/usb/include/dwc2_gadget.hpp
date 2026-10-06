/**
 * @file dwc2_gadget.hpp
 * @brief U-Boot USB Gadget framework compatible interface for DWC2 controller.
 */

#ifndef DWC2_GADGET_HPP_
#define DWC2_GADGET_HPP_

#include "uboot_compat.hpp"

namespace rk3506::usb::dwc2 {

// -------------------------------------------------------------------------
// U-Boot Endpoint Operations for DWC2
// -------------------------------------------------------------------------

int ep_enable(struct usb_ep *ep, const struct usb_endpoint_descriptor *desc) noexcept;
int ep_disable(struct usb_ep *ep) noexcept;

struct usb_request *alloc_request(struct usb_ep *ep, unsigned gfp_flags) noexcept;
void free_request(struct usb_ep *ep, struct usb_request *req) noexcept;

int queue(struct usb_ep *ep, struct usb_request *req, unsigned gfp_flags) noexcept;
int dequeue(struct usb_ep *ep, struct usb_request *req) noexcept;

int set_halt(struct usb_ep *ep, int value) noexcept;
int set_wedge(struct usb_ep *ep) noexcept;
int fifo_status(struct usb_ep *ep) noexcept;
void fifo_flush(struct usb_ep *ep) noexcept;

// -------------------------------------------------------------------------
// Core UDC Start / Stop routines
// -------------------------------------------------------------------------

int udc_start(struct usb_gadget *g, struct usb_gadget_driver *driver) noexcept;
int udc_stop(struct usb_gadget *g) noexcept;

extern struct usb_gadget_driver* active_gadget_driver;

} // namespace rk3506::usb::dwc2

#endif // DWC2_GADGET_HPP_
