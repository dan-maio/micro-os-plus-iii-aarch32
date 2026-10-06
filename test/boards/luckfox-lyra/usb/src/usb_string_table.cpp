/**
 * @file usb_string_table.cpp
 * @brief USB string descriptor management.
 *
 * This file handles the conversion of UTF-8 strings to UTF-16LE format required
 * for USB string descriptors and provides functions to retrieve strings
 * from driver-specific string tables.
 */

#include <stdint.h>
#include "uboot_compat.hpp"

/**
 * @brief Converts a UTF-8 string to a Little-Endian UTF-16 string.
 * @param s Source UTF-8 string.
 * @param cp Destination UTF-16LE buffer.
 * @param len Maximum number of characters to convert.
 * @return Number of characters converted, or -1 on encoding error.
 */
static int utf8_to_utf16le(const char *s, uint16_t *cp, unsigned len)
{
    int count = 0;
    uint8_t  c;
    uint16_t uchar;

    while (len != 0 && (c = static_cast<uint8_t>(*s++)) != 0) {
        if (c & 0x80u) {
            if ((c & 0xe0u) == 0xc0u) {
                uchar = static_cast<uint16_t>((c & 0x1fu) << 6);
                c = static_cast<uint8_t>(*s++);
                if ((c & 0xc0u) != 0x80u) goto fail;
                uchar = static_cast<uint16_t>(uchar | (c & 0x3fu));
            } else if ((c & 0xf0u) == 0xe0u) {
                uchar = static_cast<uint16_t>((c & 0x0fu) << 12);
                c = static_cast<uint8_t>(*s++);
                if ((c & 0xc0u) != 0x80u) goto fail;
                uchar = static_cast<uint16_t>(uchar | ((c & 0x3fu) << 6));
                c = static_cast<uint8_t>(*s++);
                if ((c & 0xc0u) != 0x80u) goto fail;
                uchar = static_cast<uint16_t>(uchar | (c & 0x3fu));
                if (0xd800u <= uchar && uchar <= 0xdfffu) goto fail;
            } else {
                goto fail;
            }
        } else {
            uchar = c;
        }
        put_unaligned_le16(uchar, cp++);
        ++count;
        --len;
    }
    return count;
fail:
    return -1;
}

/**
 * @brief Retrieves a USB string descriptor from a table by ID.
 * @param table Pointer to the gadget's string table.
 * @param id String index ID.
 * @param buf Output buffer for the descriptor.
 * @return Length of the descriptor in bytes, or negative error.
 */
int usb_gadget_get_string(struct usb_gadget_strings *table, int id, uint8_t *buf)
{
    struct usb_string *s;
    int len;

    if (!table)
        return -EINVAL;

    if (id == 0) {
        buf[0] = 4;
        buf[1] = USB_DT_STRING;
        buf[2] = static_cast<uint8_t>(table->language);
        buf[3] = static_cast<uint8_t>(table->language >> 8);
        return 4;
    }

    for (s = table->strings; s && s->s; ++s)
        if (s->id == id)
            break;

    if (!s || !s->s)
        return -EINVAL;

    len = static_cast<int>(min(static_cast<unsigned long>(126), strlen(s->s)));
    memset(buf + 2, 0, static_cast<unsigned long>(2 * len));
    len = utf8_to_utf16le(s->s, reinterpret_cast<uint16_t *>(&buf[2]),
                          static_cast<unsigned>(len));
    if (len < 0)
        return -EINVAL;

    buf[0] = static_cast<uint8_t>((len + 1) * 2);
    buf[1] = USB_DT_STRING;
    return buf[0];
}

