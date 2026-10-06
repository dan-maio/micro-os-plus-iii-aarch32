/**
 * @file usb_endpoint_autoconfig.cpp
 * @brief USB endpoint auto-configuration logic.
 *
 * This file implements the logic for automatically matching requested 
 * USB endpoint descriptors to available hardware endpoints based on 
 * their capabilities (transfer type, direction, etc.).
 */

#include <stdint.h>
#include "uboot_compat.hpp"

// gadget_is_dwc3 is in uboot_compat.hpp; all other gadget_is_* return 0
// gadget_chips.h: all undefined macros = 0 (already in uboot_compat.hpp)

// isdigit — used only within this file
static inline bool epa_isdigit(char c) { return c >= '0' && c <= '9'; }

static unsigned epnum;

/**
 * @brief Checks if a hardware endpoint matches the requirements of a descriptor.
 * @param gadget Pointer to the USB gadget.
 * @param ep Pointer to the hardware endpoint to check.
 * @param desc Pointer to the descriptor requirements.
 * @return 1 if matched and configured, 0 otherwise.
 */
static int ep_matches(struct usb_gadget *gadget,
                      struct usb_ep    *ep,
                      struct usb_endpoint_descriptor *desc)
{
    uint8_t type;
    const char *tmp;
    uint16_t max;

    if (ep->driver_data != nullptr)
        return 0;

    type = desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
    if (USB_ENDPOINT_XFER_CONTROL == type)
        return 0;

    if ('e' != ep->name[0])
        return 0;

    if ('-' != ep->name[2]) {
        tmp = strrchr(ep->name, '-');
        if (tmp) {
            switch (type) {
            case USB_ENDPOINT_XFER_INT:
                if ('s' == tmp[2]) return 0;   // "-iso"
                // PXA "interrupt-in" quirk omitted (gadget_is_pxa = 0)
                break;
            case USB_ENDPOINT_XFER_BULK:
                if ('b' != tmp[1]) return 0;   // != "-bulk"
                break;
            case USB_ENDPOINT_XFER_ISOC:
                if ('s' != tmp[2]) return 0;   // != "-iso"
                break;
            }
        } else {
            tmp = ep->name + strlen(ep->name);
        }
        --tmp;
        if (!epa_isdigit(*tmp)) {
            if (desc->bEndpointAddress & USB_DIR_IN) {
                if ('n' != *tmp) return 0;
            } else {
                if ('t' != *tmp) return 0;
            }
        }
    }

    max = static_cast<uint16_t>(0x7ff &
          le16_to_cpu(get_unaligned(&desc->wMaxPacketSize)));

    switch (type) {
    case USB_ENDPOINT_XFER_INT:
        if (!gadget->is_dualspeed && max > 64)
            return 0;
        /* fall through */
        [[fallthrough]];
    case USB_ENDPOINT_XFER_ISOC:
        if (ep->maxpacket < max)
            return 0;
        if (!gadget->is_dualspeed && max > 1023)
            return 0;
        if ((get_unaligned(&desc->wMaxPacketSize) &
             __constant_cpu_to_le16(3u << 11))) {
            if (!gadget->is_dualspeed)
                return 0;
        }
        break;
    }

    // MATCH — report endpoint address. Keep only the direction bit from the
    // template: a pre-set endpoint number there would OR with the hardware
    // number (e.g. template 0x01 on ep2out -> bogus 0x03).
    if (epa_isdigit(ep->name[2])) {
        uint8_t num = static_cast<uint8_t>(
            simple_strtoul(&ep->name[2], nullptr, 10));
        desc->bEndpointAddress = static_cast<uint8_t>(
            (desc->bEndpointAddress & USB_DIR_IN) | num);
    } else {
        if (++epnum > 15)
            return 0;
        desc->bEndpointAddress = static_cast<uint8_t>(
            (desc->bEndpointAddress & USB_DIR_IN) | epnum);
    }

    if (USB_ENDPOINT_XFER_BULK == type) {
        int size = ep->maxpacket;
        if (size > 64) size = 64;
        put_unaligned(cpu_to_le16(static_cast<uint16_t>(size)),
                      &desc->wMaxPacketSize);
    }
    return 1;
}

static struct usb_ep *find_ep(struct usb_gadget *gadget, const char *name)
{
    struct usb_ep *ep;
    list_for_each_entry(ep, &gadget->ep_list, ep_list) {
        if (strcmp(ep->name, name) == 0)
            return ep;
    }
    return nullptr;
}

/**
 * @brief Automatically finds and configures a suitable endpoint for a descriptor.
 * @param gadget Pointer to the USB gadget.
 * @param desc Pointer to the descriptor to be configured.
 * @return Pointer to the matched usb_ep, or nullptr if none found.
 */
struct usb_ep *usb_ep_autoconfig(struct usb_gadget *gadget,
                                  struct usb_endpoint_descriptor *desc)
{
    struct usb_ep *ep = nullptr;
    const uint8_t type = desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

    if (gadget_is_dwc3(gadget)) {
        const char *name = nullptr;
        if ((desc->bEndpointAddress & USB_DIR_IN) &&
            type == USB_ENDPOINT_XFER_BULK)
            name = "ep1in";
        else if (!(desc->bEndpointAddress & USB_DIR_IN) &&
                 type == USB_ENDPOINT_XFER_BULK)
            name = "ep1out";
        else if ((desc->bEndpointAddress & USB_DIR_IN) &&
                 type == USB_ENDPOINT_XFER_INT)
            name = "ep3in";

        if (name)
            ep = find_ep(gadget, name);
        if (ep && ep_matches(gadget, ep, desc))
            return ep;
    }

    if (gadget->ops->match_ep)
        ep = gadget->ops->match_ep(gadget, desc, nullptr);

    list_for_each_entry(ep, &gadget->ep_list, ep_list) {
        if (ep_matches(gadget, ep, desc))
            return ep;
    }

    return nullptr;
}

/**
 * @brief Resets the autoconfiguration state for all endpoints.
 * @param gadget Pointer to the USB gadget.
 */
void usb_ep_autoconfig_reset(struct usb_gadget *gadget)
{
    struct usb_ep *ep;
    list_for_each_entry(ep, &gadget->ep_list, ep_list) {
        ep->driver_data = nullptr;
    }
    epnum = 0;
}

