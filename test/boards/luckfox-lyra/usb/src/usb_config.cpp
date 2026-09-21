/**
 * @file usb_config.cpp
 * @brief USB configuration descriptor helpers.
 *
 * This file provides utility functions for constructing USB configuration 
 * and speed-dependent descriptors in memory.
 */

#include <stdint.h>
#include "uboot_compat.hpp"

/**
 * @brief Fills a buffer with a series of USB descriptors.
 * @param buf Destination buffer.
 * @param buflen Maximum length of the destination buffer.
 * @param src Null-terminated array of descriptor headers.
 * @return Total length used in the buffer, or -EINVAL on overflow.
 */
int usb_descriptor_fillbuf(void *buf, unsigned buflen,
                           const struct usb_descriptor_header **src)
{
    uint8_t *dest = static_cast<uint8_t *>(buf);

    if (!src)
        return -EINVAL;

    for (; *src != nullptr; ++src) {
        const unsigned len = (*src)->bLength;
        if (len > buflen)
            return -EINVAL;
        memcpy(dest, *src, len);
        buflen -= len;
        dest   += len;
    }
    return static_cast<int>(dest - static_cast<uint8_t *>(buf));
}

/**
 * @brief Builds a full configuration descriptor.
 * @param config Pointer to the configuration header template.
 * @param buf Destination buffer.
 * @param length Maximum length of the destination buffer.
 * @param desc Null-terminated array of functional descriptors.
 * @return Total length of the configuration descriptor, or negative error.
 */
int usb_gadget_config_buf(const struct usb_config_descriptor *config,
                          void *buf,
                          unsigned length,
                          const struct usb_descriptor_header **desc)
{
    struct usb_config_descriptor *cp =
        static_cast<struct usb_config_descriptor *>(buf);
    int len;

    if (length < USB_DT_CONFIG_SIZE || !desc)
        return -EINVAL;

    memcpy(cp, config, sizeof(*cp));

    len = usb_descriptor_fillbuf(
        static_cast<uint8_t *>(buf) + USB_DT_CONFIG_SIZE,
        length - USB_DT_CONFIG_SIZE,
        desc);
    if (len < 0)
        return len;

    len += USB_DT_CONFIG_SIZE;
    if (len > 0xffff)
        return -EINVAL;

    cp->bLength        = USB_DT_CONFIG_SIZE;
    cp->bDescriptorType = USB_DT_CONFIG;
    put_unaligned_le16(static_cast<uint16_t>(len), &cp->wTotalLength);
    cp->bmAttributes  |= USB_CONFIG_ATT_ONE;
    return len;
}

