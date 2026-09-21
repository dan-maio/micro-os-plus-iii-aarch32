/**
 * @file usb_composite.cpp
 * @brief USB Composite Gadget Infrastructure.
 *
 * This file implements the USB composite gadget framework, allowing multiple 
 * USB functions (like UMS, CDC, HID) to be combined into a single composite device.
 * It handles configuration management, descriptor building, and string mapping.
 */

#include <stdint.h>
#include "uboot_compat.hpp"

// malloc_ptr used for debug diagnostics (defined in stub.cpp)
extern uint32_t malloc_ptr;

// ---------------------------------------------------------------------------
// Local type: packed LE16 for unaligned in-place add
// ---------------------------------------------------------------------------

struct le16p_t { uint16_t val; } __attribute__((packed));

static inline void le16_add_packed(le16p_t *p, uint16_t add)
{
    p->val = cpu_to_le16(static_cast<uint16_t>(le16_to_cpu(p->val) + add));
}

// ---------------------------------------------------------------------------
// Module globals
// ---------------------------------------------------------------------------

/** Currently registered composite driver (file-scope only). */
static struct usb_composite_driver *composite = nullptr;

/**
 * Saved copy for corruption recovery; placed in .data (not BSS) so it
 * retains its value across resets.  Externally accessible so that
 * dwc3_gadget.cpp can read it via an extern declaration.
 */
__attribute__((section(".data")))
struct usb_composite_driver *composite_saved = nullptr;

// ---------------------------------------------------------------------------
// Forward declarations of static functions (needed because bos_desc and
// composite_bind call composite_setup_complete / composite_unbind)
// ---------------------------------------------------------------------------

static void composite_setup_complete(struct usb_ep *ep, struct usb_request *req);
static void composite_unbind(struct usb_gadget *gadget);
static int  composite_bind(struct usb_gadget *gadget);
static int  composite_setup(struct usb_gadget *gadget,
                             const struct usb_ctrlrequest *ctrl);
static void composite_disconnect(struct usb_gadget *gadget);
static void composite_suspend(struct usb_gadget *gadget);
static void composite_resume(struct usb_gadget *gadget);
static void reset_config(struct usb_composite_dev *cdev);

// ---------------------------------------------------------------------------
// Static helper: build config descriptor for a given speed
// ---------------------------------------------------------------------------

/**
 * @brief Fills a buffer with the configuration descriptor.
 * @param config Pointer to the USB configuration.
 * @param speed Target USB speed for descriptors.
 * @param buf Output buffer for the descriptor.
 * @param type Descriptor type (CONFIG or OTHER_SPEED_CONFIG).
 * @return Total length of the built descriptor, or negative error.
 */
static int config_buf(struct usb_configuration *config,
                      enum usb_device_speed speed, void *buf, uint8_t type)
{
    int    len   = USB_BUFSIZ - USB_DT_CONFIG_SIZE;
    uint8_t *next = static_cast<uint8_t *>(buf) + USB_DT_CONFIG_SIZE;
    struct usb_config_descriptor *c = static_cast<struct usb_config_descriptor *>(buf);
    struct usb_function *f;

    c->bLength             = USB_DT_CONFIG_SIZE;
    c->bDescriptorType     = type;
    c->bNumInterfaces      = config->next_interface_id;
    c->bConfigurationValue = config->bConfigurationValue;
    c->iConfiguration      = config->iConfiguration;
    c->bmAttributes        = static_cast<uint8_t>(USB_CONFIG_ATT_ONE | config->bmAttributes);
    c->bMaxPower           = config->bMaxPower
                             ? config->bMaxPower
                             : static_cast<uint8_t>(CONFIG_USB_GADGET_VBUS_DRAW / 2);

    if (config->descriptors) {
        int status = usb_descriptor_fillbuf(next, static_cast<unsigned>(len),
                         config->descriptors);
        if (status < 0) return status;
        len  -= status;
        next += status;
    }

    list_for_each_entry(f, &config->functions, list) {
        struct usb_descriptor_header **descriptors;
        switch (speed) {
        case USB_SPEED_SUPER: descriptors = f->ss_descriptors; break;
        case USB_SPEED_HIGH:  descriptors = f->hs_descriptors; break;
        default:              descriptors = f->descriptors;    break;
        }
        if (!descriptors)
            continue;
        int status = usb_descriptor_fillbuf(next, static_cast<unsigned>(len),
                         const_cast<const struct usb_descriptor_header **>(descriptors));
        if (status < 0) return status;
        len  -= status;
        next += status;
    }

    len = static_cast<int>(next - static_cast<uint8_t *>(buf));
    c->wTotalLength = cpu_to_le16(static_cast<uint16_t>(len));
    return len;
}

// ---------------------------------------------------------------------------
// Static helper: retrieve config descriptor for the current speed
// ---------------------------------------------------------------------------

/**
 * @brief Retrieves the configuration descriptor based on speed and index.
 * @param cdev Pointer to the composite device.
 * @param w_value Request wValue (type and index).
 * @return Length of descriptor in request buffer, or negative error.
 */
static int config_desc(struct usb_composite_dev *cdev, unsigned w_value)
{
    struct usb_gadget       *gadget = cdev->gadget;
    uint8_t                  type   = static_cast<uint8_t>(w_value >> 8);
    enum usb_device_speed    speed  = USB_SPEED_UNKNOWN;
    int                      hs     = 0;
    struct usb_configuration *c;

    if (gadget->speed == USB_SPEED_SUPER) {
        speed = gadget->speed;
    } else if (gadget_is_dualspeed(gadget)) {
        if (gadget->speed == USB_SPEED_HIGH) hs = 1;
        if (type == USB_DT_OTHER_SPEED_CONFIG) hs = !hs;
        if (hs) speed = static_cast<enum usb_device_speed>(3); // USB_SPEED_HIGH
    }

    w_value &= 0xffu;
    list_for_each_entry(c, &cdev->configs, list) {
        switch (speed) {
        case USB_SPEED_SUPER: if (!c->superspeed) continue; break;
        case USB_SPEED_HIGH:  if (!c->highspeed)  continue; break;
        default:              if (!c->fullspeed)  continue; break;
        }
        if (w_value == 0)
            return config_buf(c, speed, cdev->req->buf, type);
        --w_value;
    }
    return -EINVAL;
}

// ---------------------------------------------------------------------------
// Static helper: count configurations matching current speed
// ---------------------------------------------------------------------------

/**
 * @brief Counts configurations compatible with the requested descriptor type/speed.
 * @param cdev Pointer to the composite device.
 * @param type Descriptor type.
 * @return Count of compatible configurations.
 */
static int count_configs(struct usb_composite_dev *cdev, unsigned type)
{
    struct usb_gadget       *gadget = cdev->gadget;
    unsigned                 count  = 0;
    int                      hs     = 0;
    struct usb_configuration *c;

    if (gadget_is_dualspeed(gadget)) {
        if (gadget->speed == USB_SPEED_HIGH) hs = 1;
        if (type == USB_DT_DEVICE_QUALIFIER) hs = !hs;
    }
    list_for_each_entry(c, &cdev->configs, list) {
        if (hs) { if (!c->highspeed) continue; }
        else    { if (!c->fullspeed) continue; }
        ++count;
    }
    return static_cast<int>(count);
}

// ---------------------------------------------------------------------------
// Static helper: fill device-qualifier descriptor
// ---------------------------------------------------------------------------

/**
 * @brief Fills the device qualifier descriptor.
 * @param cdev Pointer to the composite device.
 */
static void device_qual(struct usb_composite_dev *cdev)
{
    struct usb_qualifier_descriptor *qual =
        static_cast<struct usb_qualifier_descriptor *>(cdev->req->buf);
    qual->bLength             = sizeof(*qual);
    qual->bDescriptorType     = USB_DT_DEVICE_QUALIFIER;
    qual->bcdUSB              = cdev->desc.bcdUSB;
    qual->bDeviceClass        = cdev->desc.bDeviceClass;
    qual->bDeviceSubClass     = cdev->desc.bDeviceSubClass;
    qual->bDeviceProtocol     = cdev->desc.bDeviceProtocol;
    qual->bMaxPacketSize0     = static_cast<uint8_t>(cdev->gadget->ep0->maxpacket);
    qual->bNumConfigurations  = static_cast<uint8_t>(
                                    count_configs(cdev, USB_DT_DEVICE_QUALIFIER));
    qual->bRESERVED           = 0;
}

// ---------------------------------------------------------------------------
// Static helper: disable all functions in the active configuration
// ---------------------------------------------------------------------------

static void reset_config(struct usb_composite_dev *cdev)
{
    struct usb_function *f;
    list_for_each_entry(f, &cdev->config->functions, list) {
        if (f->disable) f->disable(f);
        bitmap_zero(f->endpoints, 32);
    }
    cdev->config = nullptr;
}

// ---------------------------------------------------------------------------
// Static helper: apply a numbered configuration
// ---------------------------------------------------------------------------

/**
 * @brief Switches the device to a specific configuration.
 * @param cdev Pointer to the composite device.
 * @param ctrl Pointer to the control request.
 * @param number Configuration number to select.
 * @return 0 on success, or negative error.
 */
static int set_config(struct usb_composite_dev *cdev,
                      const struct usb_ctrlrequest * /*ctrl*/, unsigned number)
{
    struct usb_gadget        *gadget = cdev->gadget;
    unsigned                  power  = gadget_is_otg(gadget) ? 8u : 100u;
    int                       result = -EINVAL;
    struct usb_configuration *c      = nullptr;

    if (cdev->config)
        reset_config(cdev);

    if (number) {
        list_for_each_entry(c, &cdev->configs, list) {
            if (c->bConfigurationValue == number) { result = 0; break; }
        }
        if (result < 0) goto done;
    } else {
        result = 0;
    }

    // debug (GCC statement expression removed — debug() is a no-op)

    if (!c) goto done;
    cdev->config = c;

    for (int tmp = 0; tmp < MAX_CONFIG_INTERFACES; ++tmp) {
        struct usb_function *f = c->interface[tmp];
        if (!f) break;

        struct usb_descriptor_header **descriptors;
        switch (gadget->speed) {
        case USB_SPEED_SUPER: descriptors = f->ss_descriptors; break;
        case USB_SPEED_HIGH:  descriptors = f->hs_descriptors; break;
        default:              descriptors = f->descriptors;    break;
        }

        for (; descriptors && *descriptors; ++descriptors) {
            if ((*descriptors)->bDescriptorType != USB_DT_ENDPOINT) continue;
            struct usb_endpoint_descriptor *ep =
                reinterpret_cast<struct usb_endpoint_descriptor *>(*descriptors);
            int addr = static_cast<int>(
                ((ep->bEndpointAddress & 0x80u) >> 3) |
                 (ep->bEndpointAddress & 0x0fu));
            generic_set_bit(addr, f->endpoints);
        }

        result = f->set_alt(f, static_cast<unsigned>(tmp), 0);
        if (result < 0) {
            reset_config(cdev);
            goto done;
        }
    }

    power = c->bMaxPower
            ? static_cast<unsigned>(2 * c->bMaxPower)
            : static_cast<unsigned>(CONFIG_USB_GADGET_VBUS_DRAW);
done:
    usb_gadget_vbus_draw(gadget, power);
    return result;
}

// ---------------------------------------------------------------------------
// Static helper: collect unique language IDs
// ---------------------------------------------------------------------------

static void collect_langs(struct usb_gadget_strings **sp, void *buf)
{
    le16p_t *end = reinterpret_cast<le16p_t *>(static_cast<uint8_t *>(buf) + 252);

    while (*sp) {
        uint16_t language = cpu_to_le16((*sp)->language);
        le16p_t *tmp = static_cast<le16p_t *>(buf);
        while (tmp->val && tmp < end) {
            if (tmp->val == language) goto repeat;
            ++tmp;
        }
        tmp->val = language;
repeat:
        ++sp;
    }
}

// ---------------------------------------------------------------------------
// Static helper: search string tables for a specific ID + language
// ---------------------------------------------------------------------------

static int lookup_string(struct usb_gadget_strings **sp,
                         void *buf, uint16_t language, int id)
{
    while (*sp) {
        struct usb_gadget_strings *s = *sp++;
        if (s->language != language) continue;
        int value = usb_gadget_get_string(s, id, static_cast<uint8_t *>(buf));
        if (value > 0) return value;
    }
    return -EINVAL;
}

// ---------------------------------------------------------------------------
// Static helper: retrieve string descriptor by ID and language
// ---------------------------------------------------------------------------

/**
 * @brief Retrieves a string descriptor by index and language.
 * @param cdev Pointer to the composite device.
 * @param buf Output buffer for the string descriptor.
 * @param language Target language ID.
 * @param id String index ID.
 * @return Length of string descriptor, or negative error.
 */
static int get_string(struct usb_composite_dev *cdev,
                      void *buf, uint16_t language, int id)
{
    struct usb_string_descriptor *s =
        static_cast<struct usb_string_descriptor *>(buf);
    struct usb_configuration     *c;
    struct usb_function          *f;

    if (id == 0) {
        memset(s, 0, 256);
        s->bDescriptorType = USB_DT_STRING;

        if (composite->strings)
            collect_langs(composite->strings, s->wData);

        list_for_each_entry(c, &cdev->configs, list) {
            if (c->strings) collect_langs(c->strings, s->wData);
            list_for_each_entry(f, &c->functions, list) {
                if (f->strings) collect_langs(f->strings, s->wData);
            }
        }

        int len = 0;
        while (len <= 126 && s->wData[len]) ++len;
        if (!len) return -EINVAL;
        s->bLength = static_cast<uint8_t>(2 * (len + 1));
        return s->bLength;
    }

    // Look up the string in driver, config, and function tables
    if (composite->strings) {
        int len = lookup_string(composite->strings, buf, language, id);
        if (len > 0) return len;
    }
    list_for_each_entry(c, &cdev->configs, list) {
        if (c->strings) {
            int len = lookup_string(c->strings, buf, language, id);
            if (len > 0) return len;
        }
        list_for_each_entry(f, &c->functions, list) {
            if (!f->strings) continue;
            int len = lookup_string(f->strings, buf, language, id);
            if (len > 0) return len;
        }
    }
    return -EINVAL;
}

// ---------------------------------------------------------------------------
// Static helper: EP0 completion handler for setup responses
// ---------------------------------------------------------------------------

static void composite_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
    (void)ep;
    if (req->status || req->actual != req->length) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "setup complete --> %d, %d/%d\n",
                 req->status, req->actual, req->length);
        uart_puts(buf);
    }
}

// ---------------------------------------------------------------------------
// Static helper: build Binary Object Store (BOS) descriptor
// ---------------------------------------------------------------------------

/**
 * @brief Builds the Binary Object Store (BOS) descriptor.
 * @param cdev Pointer to the composite device.
 * @return Length of built BOS descriptor.
 */
static int bos_desc(struct usb_composite_dev *cdev)
{
    struct usb_bos_descriptor *bos =
        static_cast<struct usb_bos_descriptor *>(cdev->req->buf);

    bos->bLength          = USB_DT_BOS_SIZE;
    bos->bDescriptorType  = USB_DT_BOS;
    bos->wTotalLength     = cpu_to_le16(USB_DT_BOS_SIZE);
    bos->bNumDeviceCaps   = 0;

    if (cdev->gadget->speed < USB_SPEED_SUPER) {
        // Minimal capability descriptor for rockusb bcdUSB=0x0201
        struct usb_dev_cap_header *cap =
            reinterpret_cast<struct usb_dev_cap_header *>(
                reinterpret_cast<uint8_t *>(cdev->req->buf) +
                le16_to_cpu(bos->wTotalLength));
        ++bos->bNumDeviceCaps;
        bos->wTotalLength = cpu_to_le16(
            static_cast<uint16_t>(le16_to_cpu(bos->wTotalLength) + sizeof(*cap)));
        cap->bLength             = sizeof(*cap);
        cap->bDescriptorType     = USB_DT_DEVICE_CAPABILITY;
        cap->bDevCapabilityType  = 0;
    } else {
        // USB 2.0 Extension
        struct usb_ext_cap_descriptor *usb_ext =
            reinterpret_cast<struct usb_ext_cap_descriptor *>(
                reinterpret_cast<uint8_t *>(cdev->req->buf) +
                le16_to_cpu(bos->wTotalLength));
        ++bos->bNumDeviceCaps;
        le16_add_packed(reinterpret_cast<le16p_t *>(&bos->wTotalLength),
                        USB_DT_USB_EXT_CAP_SIZE);
        usb_ext->bLength             = USB_DT_USB_EXT_CAP_SIZE;
        usb_ext->bDescriptorType     = USB_DT_DEVICE_CAPABILITY;
        usb_ext->bDevCapabilityType  = USB_CAP_TYPE_EXT;
        usb_ext->bmAttributes        = USB_LPM_SUPPORT;

        // SuperSpeed USB Capability
        struct usb_ss_cap_descriptor *ss_cap =
            reinterpret_cast<struct usb_ss_cap_descriptor *>(
                reinterpret_cast<uint8_t *>(cdev->req->buf) +
                le16_to_cpu(bos->wTotalLength));
        ++bos->bNumDeviceCaps;
        le16_add_packed(reinterpret_cast<le16p_t *>(&bos->wTotalLength),
                        USB_DT_USB_SS_CAP_SIZE);
        ss_cap->bLength                = USB_DT_USB_SS_CAP_SIZE;
        ss_cap->bDescriptorType        = USB_DT_DEVICE_CAPABILITY;
        ss_cap->bDevCapabilityType     = USB_SS_CAP_TYPE;
        ss_cap->bmAttributes           = 0;
        ss_cap->wSpeedSupported        = cpu_to_le16(
            static_cast<uint16_t>(USB_FULL_SPEED_OPERATION |
                                  USB_HIGH_SPEED_OPERATION |
                                  USB_5GBPS_OPERATION));
        ss_cap->bFunctionalitySupport  = USB_FULL_SPEED_OPERATION;
        ss_cap->bU1devExitLat          = USB_DEFAULT_U1_DEV_EXIT_LAT;
        ss_cap->bU2DevExitLat          = cpu_to_le16(
            static_cast<uint16_t>(USB_DEFAULT_U2_DEV_EXIT_LAT));
    }

    return static_cast<int>(le16_to_cpu(bos->wTotalLength));
}

// ---------------------------------------------------------------------------
// Static: main EP0 setup handler
// ---------------------------------------------------------------------------

/**
 * @brief Primary endpoint 0 setup request handler for composite devices.
 * @param gadget Pointer to the USB gadget.
 * @param ctrl Pointer to the control request.
 * @return 0 on success, or negative error.
 */
static int composite_setup(struct usb_gadget *gadget,
                            const struct usb_ctrlrequest *ctrl)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(get_gadget_data(gadget));
    uint16_t w_length = le16_to_cpu(ctrl->wLength);
    uint16_t w_index  = le16_to_cpu(ctrl->wIndex);
    uint16_t w_value  = le16_to_cpu(ctrl->wValue);
    uint8_t  intf     = static_cast<uint8_t>(w_index & 0xFFu);
    int      value    = -EOPNOTSUPP;
    struct usb_request       *req  = cdev->req;
    struct usb_function      *f    = nullptr;
    struct usb_configuration *c;
    uint8_t  endp;

    req->zero     = 0;
    req->complete = composite_setup_complete;
    req->length   = USB_BUFSIZ;
    gadget->ep0->driver_data = cdev;

    bool standard = ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD);
    if (!standard) goto unknown;

    switch (ctrl->bRequest) {

    case USB_REQ_GET_DESCRIPTOR:
        if (ctrl->bRequestType != USB_DIR_IN) goto unknown;
        switch (w_value >> 8) {

        case USB_DT_DEVICE:
            cdev->desc.bNumConfigurations =
                static_cast<uint8_t>(count_configs(cdev, USB_DT_DEVICE));
            if (gadget_is_superspeed(gadget) &&
                gadget->speed >= USB_SPEED_SUPER) {
#ifndef CONFIG_SUPPORT_USBPLUG
                if (!strncmp(cdev->driver->name, "rkusb_ums_dnl", 13))
                    cdev->desc.bcdUSB = cpu_to_le16(0x0301);
                else
                    cdev->desc.bcdUSB = cpu_to_le16(0x0300);
#else
                cdev->desc.bcdUSB = cpu_to_le16(0x0300);
#endif
                cdev->desc.bMaxPacketSize0 = 9;
            } else {
                cdev->desc.bMaxPacketSize0 =
                    static_cast<uint8_t>(cdev->gadget->ep0->maxpacket);
            }
            value = min(static_cast<int>(w_length), static_cast<int>(sizeof(cdev->desc)));
            memcpy(req->buf, &cdev->desc, static_cast<unsigned long>(value));
            {
                uint8_t *p = static_cast<uint8_t *>(req->buf);
                char _h[96];
                snprintf(_h, sizeof(_h),
                    "DEV_DESC: %02x %02x %02x%02x %02x %02x %02x %02x "
                    "%02x%02x %02x%02x %02x %02x %02x %02x %02x %02x\n",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                    p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
                    p[16], p[17]);
                uart_puts(_h);
            }
            break;

        case USB_DT_DEVICE_QUALIFIER:
            if (!gadget_is_dualspeed(gadget)) break;
            device_qual(cdev);
            value = min(static_cast<int>(w_length), static_cast<int>(sizeof(struct usb_qualifier_descriptor)));
            break;

        case USB_DT_OTHER_SPEED_CONFIG:
            if (!gadget_is_dualspeed(gadget)) break;
            [[fallthrough]];
        case USB_DT_CONFIG:
            value = config_desc(cdev, w_value);
            if (value >= 0) {
                uint8_t *_p   = static_cast<uint8_t *>(req->buf);
                int      _len = value < 32 ? value : 32;
                char     _h[128];
                snprintf(_h, sizeof(_h),
                    "CFG_DESC[%d]: %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x",
                    value,
                    _len > 0  ? _p[0]  : 0, _len > 1  ? _p[1]  : 0,
                    _len > 2  ? _p[2]  : 0, _len > 3  ? _p[3]  : 0,
                    _len > 4  ? _p[4]  : 0, _len > 5  ? _p[5]  : 0,
                    _len > 6  ? _p[6]  : 0, _len > 7  ? _p[7]  : 0,
                    _len > 8  ? _p[8]  : 0, _len > 9  ? _p[9]  : 0,
                    _len > 10 ? _p[10] : 0, _len > 11 ? _p[11] : 0,
                    _len > 12 ? _p[12] : 0, _len > 13 ? _p[13] : 0,
                    _len > 14 ? _p[14] : 0, _len > 15 ? _p[15] : 0);
                uart_puts(_h);
                if (value > 16) {
                    snprintf(_h, sizeof(_h),
                        " %02x %02x %02x %02x %02x %02x %02x %02x "
                        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        _len > 16 ? _p[16] : 0, _len > 17 ? _p[17] : 0,
                        _len > 18 ? _p[18] : 0, _len > 19 ? _p[19] : 0,
                        _len > 20 ? _p[20] : 0, _len > 21 ? _p[21] : 0,
                        _len > 22 ? _p[22] : 0, _len > 23 ? _p[23] : 0,
                        _len > 24 ? _p[24] : 0, _len > 25 ? _p[25] : 0,
                        _len > 26 ? _p[26] : 0, _len > 27 ? _p[27] : 0,
                        _len > 28 ? _p[28] : 0, _len > 29 ? _p[29] : 0,
                        _len > 30 ? _p[30] : 0, _len > 31 ? _p[31] : 0);
                    uart_puts(_h);
                } else {
                    uart_puts("\n");
                }
                value = min(static_cast<int>(w_length), value);
            }
            break;

        case USB_DT_STRING:
            value = get_string(cdev, req->buf,
                               static_cast<uint16_t>(w_index),
                               static_cast<int>(w_value & 0xffu));
            if (value >= 0) {
                uint8_t *_sp = static_cast<uint8_t *>(req->buf);
                char     _sh[48];
                snprintf(_sh, sizeof(_sh),
                    "STR_DESC: id=%d lang=%d len=%d [%02x %02x %02x %02x]\n",
                    w_value & 0xffu, w_index, value,
                    value > 0 ? _sp[0] : 0, value > 1 ? _sp[1] : 0,
                    value > 2 ? _sp[2] : 0, value > 3 ? _sp[3] : 0);
                uart_puts(_sh);
                value = min(static_cast<int>(w_length), value);
            }
            break;

        case USB_DT_BOS:
            if (gadget_is_superspeed(gadget) ||
                !strncmp(cdev->driver->name, "rkusb_ums_dnl", 13)) {
                value = bos_desc(cdev);
                value = min(static_cast<int>(w_length), value);
            }
            break;

        default:
            goto unknown;
        }
        break;

    case USB_REQ_SET_CONFIGURATION:
        if (ctrl->bRequestType != 0) goto unknown;
        {
            char _sc[48];
            snprintf(_sc, sizeof(_sc),
                "SET_CONFIG: val=%d state=%d\n",
                w_value, gadget->state);
            uart_puts(_sc);
        }
        if (gadget_is_otg(gadget)) {
            if (gadget->a_hnp_support)      { /* HNP available */ }
            else if (gadget->a_alt_hnp_support) { /* HNP on another port */ }
            else                            { /* HNP inactive */ }
        }
        value = set_config(cdev, ctrl, static_cast<unsigned>(w_value));
        {
            char _sc2[48];
            snprintf(_sc2, sizeof(_sc2),
                "SET_CONFIG: ret=%d config=%p\n",
                value, static_cast<void *>(cdev->config));
            uart_puts(_sc2);
        }
        break;

    case USB_REQ_GET_CONFIGURATION:
        if (ctrl->bRequestType != USB_DIR_IN) goto unknown;
        *static_cast<uint8_t *>(req->buf) =
            cdev->config
            ? cdev->config->bConfigurationValue
            : static_cast<uint8_t>(0);
        value = min(static_cast<int>(w_length), 1);
        break;

    case USB_REQ_SET_INTERFACE:
        if (ctrl->bRequestType != USB_RECIP_INTERFACE) goto unknown;
        if (!cdev->config || w_index >= MAX_CONFIG_INTERFACES) break;
        f = cdev->config->interface[intf];
        if (!f) break;
        if (w_value && !f->set_alt) break;
        value = f->set_alt(f, static_cast<unsigned>(w_index),
                           static_cast<unsigned>(w_value));
        break;

    case USB_REQ_GET_INTERFACE:
        if (ctrl->bRequestType != (USB_DIR_IN | USB_RECIP_INTERFACE)) goto unknown;
        if (!cdev->config || w_index >= MAX_CONFIG_INTERFACES) break;
        f = cdev->config->interface[intf];
        if (!f) break;
        value = f->get_alt ? f->get_alt(f, static_cast<unsigned>(w_index)) : 0;
        if (value < 0) break;
        *static_cast<uint8_t *>(req->buf) = static_cast<uint8_t>(value);
        value = min(static_cast<int>(w_length), 1);
        break;

    default:
unknown:
        if (!cdev->config) goto done;

        switch (ctrl->bRequestType & USB_RECIP_MASK) {
        case USB_RECIP_INTERFACE:
            if (!cdev->config) break;
            f = cdev->config->interface[intf];
            break;

        case USB_RECIP_ENDPOINT:
            if (!cdev->config) break;
            endp = static_cast<uint8_t>(
                ((w_index & 0x80u) >> 3) | (w_index & 0x0fu));
            list_for_each_entry(f, &cdev->config->functions, list) {
                if (test_bit(endp, f->endpoints)) break;
            }
            if (&f->list == &cdev->config->functions)
                f = nullptr;
            break;

        case USB_RECIP_DEVICE:
            if (cdev->config && cdev->config->next_interface_id == 1)
                f = cdev->config->interface[intf];
            break;
        }

        if (f && f->setup)
            value = f->setup(f, ctrl);
        else {
            c = cdev->config;
            if (c && c->setup) value = c->setup(c, ctrl);
        }
        goto done;
    }

    if (value >= 0) {
        req->length = static_cast<unsigned>(value);
        req->zero   = (value < w_length) ? 1u : 0u;
        value = usb_ep_queue(gadget->ep0, req, GFP_KERNEL);
        if (value < 0) {
            req->status = 0;
            composite_setup_complete(gadget->ep0, req);
        }
    }
done:
    return value;
}

// ---------------------------------------------------------------------------
// Static: handle USB disconnect / reset
// ---------------------------------------------------------------------------

/**
 * @brief Handles USB disconnect and reset events for composite devices.
 * @param gadget Pointer to the USB gadget.
 */
static void composite_disconnect(struct usb_gadget *gadget)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(get_gadget_data(gadget));

    if (!composite) {
        uart_puts("composite_disconnect: composite=NULL!\n");
        return;
    }
    if (reinterpret_cast<uintptr_t>(composite) < 0x00200000UL ||
        reinterpret_cast<uintptr_t>(composite) > 0x00300000UL) {
        char _buf[80];
        snprintf(_buf, sizeof(_buf),
            "composite_disconnect: composite=%p CORRUPT, restoring from %p\n",
            static_cast<void *>(composite),
            static_cast<void *>(composite_saved));
        uart_puts(_buf);
        composite = composite_saved;
        if (!composite) return;
    }

    if (cdev->config)
        reset_config(cdev);
    if (composite->disconnect)
        composite->disconnect(cdev);
}

// ---------------------------------------------------------------------------
// Static: tear down the composite device
// ---------------------------------------------------------------------------

/**
 * @brief Tears down and releases resources for the composite device.
 * @param gadget Pointer to the USB gadget.
 */
static void composite_unbind(struct usb_gadget *gadget)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(get_gadget_data(gadget));

    BUG_ON(cdev->config);

    while (!list_empty(&cdev->configs)) {
        struct usb_configuration *c =
            list_first_entry(&cdev->configs, struct usb_configuration, list);
        while (!list_empty(&c->functions)) {
            struct usb_function *f =
                list_first_entry(&c->functions, struct usb_function, list);
            list_del(&f->list);
            if (f->unbind) f->unbind(c, f);
        }
        list_del(&c->list);
        if (c->unbind) c->unbind(c);
        free(c);
    }
    if (composite && composite->unbind)
        composite->unbind(cdev);

    if (cdev->req) {
        kfree(cdev->req->buf);
        usb_ep_free_request(gadget->ep0, cdev->req);
    }
    kfree(cdev);
    set_gadget_data(gadget, nullptr);
    composite = nullptr;
}

// ---------------------------------------------------------------------------
// Static: initialize and bind the composite device
// ---------------------------------------------------------------------------

/**
 * @brief Initializes and binds the composite device to the gadget hardware.
 * @param gadget Pointer to the USB gadget.
 * @return 0 on success, or -ENOMEM on failure.
 */
static int composite_bind(struct usb_gadget *gadget)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(calloc(sizeof(*cdev), 1));
    uart_puts("composite_bind: calloc done\n");
    if (!cdev) return -ENOMEM;

    cdev->gadget = gadget;
    set_gadget_data(gadget, cdev);
    INIT_LIST_HEAD(&cdev->configs);

    // Preallocate EP0 control request and buffer
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "composite: ep0=%p ops=%p\n",
            static_cast<void *>(gadget->ep0),
            gadget->ep0 ? reinterpret_cast<const void *>(gadget->ep0->ops) : nullptr);
        uart_puts(buf);
        if (!gadget->ep0) {
            uart_puts("composite: FATAL - ep0 NULL\n");
            goto fail;
        }
        if (!gadget->ep0->ops) {
            uart_puts("composite: FATAL - ops NULL\n");
            goto fail;
        }
        snprintf(buf, sizeof(buf), "composite: alloc_req=%p pool=%d\n",
            reinterpret_cast<void *>(
                reinterpret_cast<uintptr_t>(gadget->ep0->ops->alloc_request)),
            malloc_ptr ? 1 : 0);
        uart_puts(buf);
    }

    cdev->req = usb_ep_alloc_request(gadget->ep0, GFP_KERNEL);
    if (!cdev->req) goto fail;
    cdev->req->buf = memalign(CONFIG_SYS_CACHELINE_SIZE, USB_BUFSIZ);
    if (!cdev->req->buf) goto fail;
    cdev->req->complete = composite_setup_complete;
    gadget->ep0->driver_data = cdev;

    cdev->bufsiz = USB_BUFSIZ;
    cdev->driver = composite;

    usb_gadget_set_selfpowered(gadget);
    usb_ep_autoconfig_reset(cdev->gadget);

    uart_puts("composite_bind: calling composite->bind\n");
    {
        int status = composite->bind(cdev);
        uart_puts("composite_bind: returned from composite->bind\n");
        if (status < 0) goto fail;
    }

    memcpy(&cdev->desc, composite->dev,
           sizeof(struct usb_device_descriptor));
    cdev->desc.bMaxPacketSize0 = static_cast<uint8_t>(gadget->ep0->maxpacket);

    return 0;

fail:
    composite_unbind(gadget);
    return -ENOMEM;
}

// ---------------------------------------------------------------------------
// Static: suspend / resume
// ---------------------------------------------------------------------------

static void composite_suspend(struct usb_gadget *gadget)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(get_gadget_data(gadget));
    struct usb_function *f;

    if (!composite) {
        uart_puts("composite_suspend: composite=NULL!\n");
        return;
    }
    if (reinterpret_cast<uintptr_t>(composite) < 0x00200000UL ||
        reinterpret_cast<uintptr_t>(composite) > 0x00300000UL) {
        char _buf[80];
        snprintf(_buf, sizeof(_buf),
            "composite_suspend: composite=%p CORRUPT, restoring from %p\n",
            static_cast<void *>(composite),
            static_cast<void *>(composite_saved));
        uart_puts(_buf);
        composite = composite_saved;
        if (!composite) return;
    }
    if (cdev->config) {
        list_for_each_entry(f, &cdev->config->functions, list) {
            if (f->suspend) f->suspend(f);
        }
    }
    if (composite->suspend) composite->suspend(cdev);
    cdev->suspended = 1;
}

static void composite_resume(struct usb_gadget *gadget)
{
    struct usb_composite_dev *cdev =
        static_cast<struct usb_composite_dev *>(get_gadget_data(gadget));
    struct usb_function *f;

    if (!composite) {
        uart_puts("composite_resume: composite=NULL!\n");
        return;
    }
    if (reinterpret_cast<uintptr_t>(composite) < 0x00200000UL ||
        reinterpret_cast<uintptr_t>(composite) > 0x00300000UL) {
        char _buf[80];
        snprintf(_buf, sizeof(_buf),
            "composite_resume: composite=%p CORRUPT, restoring from %p\n",
            static_cast<void *>(composite),
            static_cast<void *>(composite_saved));
        uart_puts(_buf);
        composite = composite_saved;
        if (!composite) return;
    }
    if (composite->resume) composite->resume(cdev);
    if (cdev->config) {
        list_for_each_entry(f, &cdev->config->functions, list) {
            if (f->resume) f->resume(f);
        }
    }
    cdev->suspended = 0;
}

// ---------------------------------------------------------------------------
// Static gadget driver descriptor
// ---------------------------------------------------------------------------

static struct usb_gadget_driver composite_driver = {
    .function   = nullptr,
    .speed      = static_cast<enum usb_device_speed>(3), // USB_SPEED_HIGH
    .dev        = nullptr,  // set in usb_composite_register()
    .strings    = nullptr,  // set in usb_composite_register()
    .bind       = composite_bind,
    .unbind     = composite_unbind,
    .setup      = composite_setup,
    .disconnect = composite_disconnect,
    .suspend    = composite_suspend,
    .resume     = composite_resume,
    .reset      = composite_disconnect,
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Public API

/**
 * @brief Adds a USB function to a configuration.
 * @param config Pointer to the USB configuration.
 * @param function Pointer to the USB function implementation.
 * @return 0 on success, or negative error.
 */
int usb_add_function(struct usb_configuration *config,
                     struct usb_function *function)
{
    int value = -EINVAL;

    if (!function->set_alt || !function->disable)
        goto done;

    function->config = config;
    list_add_tail(&function->list, &config->functions);

    if (function->bind) {
        value = function->bind(config, function);
        if (value < 0) {
            list_del(&function->list);
            function->config = nullptr;
        }
    } else {
        value = 0;
    }

    if (!config->fullspeed  && function->descriptors)    config->fullspeed  = 1;
    if (!config->highspeed  && function->hs_descriptors) config->highspeed  = 1;
    if (!config->superspeed && function->ss_descriptors) config->superspeed = 1;

done:
    return value;
}

int usb_function_deactivate(struct usb_function *function)
{
    struct usb_composite_dev *cdev = function->config->cdev;
    int status = 0;
    if (cdev->deactivations == 0)
        status = usb_gadget_disconnect(cdev->gadget);
    if (status == 0)
        ++cdev->deactivations;
    return status;
}

int usb_function_activate(struct usb_function *function)
{
    struct usb_composite_dev *cdev = function->config->cdev;
    int status = 0;
    if (cdev->deactivations == 0) {
        status = -EINVAL;
    } else {
        --cdev->deactivations;
        if (cdev->deactivations == 0)
            status = usb_gadget_connect(cdev->gadget);
    }
    return status;
}

int usb_interface_id(struct usb_configuration *config,
                     struct usb_function *function)
{
    uint8_t id = config->next_interface_id;
    if (id < MAX_CONFIG_INTERFACES) {
        config->interface[id] = function;
        config->next_interface_id = static_cast<uint8_t>(id + 1);
        return id;
    }
    return -ENODEV;
}

/**
 * @brief Adds a USB configuration to the composite device.
 * @param cdev Pointer to the composite device.
 * @param config Pointer to the USB configuration.
 * @return 0 on success, or negative error.
 */
int usb_add_config(struct usb_composite_dev *cdev,
                   struct usb_configuration *config)
{
    int status = -EINVAL;
    struct usb_configuration *c;

    if (!config->bConfigurationValue || !config->bind)
        goto done;

    list_for_each_entry(c, &cdev->configs, list) {
        if (c->bConfigurationValue == config->bConfigurationValue) {
            status = -EBUSY;
            goto done;
        }
    }

    config->cdev = cdev;
    list_add_tail(&config->list, &cdev->configs);
    INIT_LIST_HEAD(&config->functions);
    config->next_interface_id = 0;

    status = config->bind(config);
    if (status < 0) {
        list_del(&config->list);
        config->cdev = nullptr;
    }

    usb_ep_autoconfig_reset(cdev->gadget);

done:
    return status;
}

int usb_string_id(struct usb_composite_dev *cdev)
{
    if (cdev->next_string_id < 254) {
        ++cdev->next_string_id;
        return cdev->next_string_id;
    }
    return -ENODEV;
}

int usb_string_ids_tab(struct usb_composite_dev *cdev, struct usb_string *str)
{
    uint8_t next = cdev->next_string_id;
    for (; str->s; ++str) {
        if (next >= 254) return -ENODEV;
        str->id = ++next;
    }
    cdev->next_string_id = next;
    return 0;
}

int usb_string_ids_n(struct usb_composite_dev *c, unsigned n)
{
    uint8_t next = c->next_string_id;
    if (n > 254 || next + n > 254) return -ENODEV;
    c->next_string_id = static_cast<uint8_t>(c->next_string_id + n);
    return static_cast<int>(next + 1);
}

/**
 * @brief Registers a composite driver with the UDC core.
 * @param driver Pointer to the composite driver descriptor.
 * @return 0 on success, or negative error code.
 */
int usb_composite_register(struct usb_composite_driver *driver)
{
    int res;

    uart_puts("composite: checking params\n");
    if (!driver)          { uart_puts("composite: FAIL - driver is NULL\n"); return -EINVAL; }
    if (!driver->dev)     { uart_puts("composite: FAIL - driver->dev is NULL\n"); return -EINVAL; }
    if (!driver->bind)    { uart_puts("composite: FAIL - driver->bind is NULL\n"); return -EINVAL; }
    if (composite)        { uart_puts("composite: FAIL - composite already set\n"); return -EINVAL; }
    uart_puts("composite: all params OK\n");

    if (!driver->name) driver->name = "composite";
    composite       = driver;
    composite_saved = driver;

    composite_driver.dev     = driver->dev;
    composite_driver.strings = driver->strings;

    {
        char _buf[64];
        snprintf(_buf, sizeof(_buf), "composite: copied dev=%p strings=%p\n",
            static_cast<const void *>(composite_driver.dev),
            static_cast<void *>(composite_driver.strings));
        uart_puts(_buf);
    }

    uart_puts("composite: calling usb_gadget_register_driver\n");
    res = usb_gadget_register_driver(&composite_driver);
    if (res != 0) {
        uart_puts("composite: register_driver FAILED\n");
        composite = nullptr;
    }
    return res;
}

/**
 * @brief Unregisters a composite driver.
 * @param driver Pointer to the composite driver descriptor.
 */
void usb_composite_unregister(struct usb_composite_driver *driver)
{
    if (composite != driver) return;
    usb_gadget_unregister_driver(&composite_driver);
    composite = nullptr;
}

