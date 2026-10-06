#include <stdint.h>
#include "uboot_compat.hpp"
#include "bit_field.hpp"

extern "C" void uart_puts(const char* s);
extern "C" void uart_putc(char c);
extern "C" char* hex32(uint32_t val);

// These are DEFINED in main.cpp
extern "C" uint8_t* g_file_buf;
extern "C" uint32_t g_file_max;
extern "C" uint32_t g_file_len;
extern "C" volatile int g_file_ready;

void usb_vendor_read(void *buf, int len);

bool device_connected = false;
bool endpoints_enabled = false;
bool read_pending = false;

static struct usb_ep *ep_in, *ep_out;
static char cmd_buf[512] __attribute__((aligned(64), section(".dma_nc")));
static char data_buf[65536] __attribute__((aligned(64), section(".dma_nc")));
static char echo_buf[65536] __attribute__((aligned(64), section(".dma_nc")));
static char ack_buf[7] __attribute__((aligned(64), section(".dma_nc")));
static uint32_t xfer_total = 0;
static int xfer_active = 0;

static constexpr uint8_t CMD_ECHO = 0x01;
static constexpr uint8_t CMD_BIG = 0x02;

static struct usb_interface_descriptor vendor_intf = {
    .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0, .bAlternateSetting = 0, .bNumEndpoints = 2,
    .bInterfaceClass = 0xff, .bInterfaceSubClass = 0x00, .bInterfaceProtocol = 0x00,
};
static struct usb_endpoint_descriptor vendor_fs_in_ep = {
    .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(64),
};
static struct usb_endpoint_descriptor vendor_fs_out_ep = {
    .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(64),
};
static struct usb_endpoint_descriptor vendor_hs_in_ep = {
    .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(512),
};
static struct usb_endpoint_descriptor vendor_hs_out_ep = {
    .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(512),
};
static struct usb_descriptor_header *vendor_fs_descs[] = {
    (usb_descriptor_header*)&vendor_intf, (usb_descriptor_header*)&vendor_fs_in_ep,
    (usb_descriptor_header*)&vendor_fs_out_ep, nullptr,
};
static struct usb_descriptor_header *vendor_hs_descs[] = {
    (usb_descriptor_header*)&vendor_intf, (usb_descriptor_header*)&vendor_hs_in_ep,
    (usb_descriptor_header*)&vendor_hs_out_ep, nullptr,
};
static struct usb_device_descriptor device_desc = {
    .bLength = sizeof(device_desc), .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = cpu_to_le16(0x0200), .bDeviceClass = 0, .bDeviceSubClass = 0,
    .bDeviceProtocol = 0, .idVendor = cpu_to_le16(0x2207), .idProduct = cpu_to_le16(0x0019),
    .bcdDevice = cpu_to_le16(0x0100), .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1,
};
static struct usb_string strings[] = {
    { 1, "Luckfox" }, { 2, "Lyra USB-SD Logger" }, { 3, "12345678" }, {},
};
static struct usb_gadget_strings stringtab_lang = { .language = 0x0409, .strings = strings };
static struct usb_gadget_strings *stringtab[] = { &stringtab_lang, nullptr };
static struct usb_function vendor_f;
static struct usb_configuration vendor_config;
static struct usb_composite_driver vendor_driver;

static int vendor_bind(struct usb_configuration *c, struct usb_function *f) {
    int id = usb_interface_id(c, f);
    if (id < 0) return id;
    vendor_intf.bInterfaceNumber = (uint8_t)id;
    ep_in = usb_ep_autoconfig(c->cdev->gadget, &vendor_fs_in_ep);
    ep_out = usb_ep_autoconfig(c->cdev->gadget, &vendor_fs_out_ep);
    if (!ep_in || !ep_out) return -ENODEV;
    ep_in->driver_data = f; ep_out->driver_data = f;
    vendor_hs_in_ep.bEndpointAddress = vendor_fs_in_ep.bEndpointAddress;
    vendor_hs_out_ep.bEndpointAddress = vendor_fs_out_ep.bEndpointAddress;
    return 0;
}

static int vendor_set_alt(struct usb_function *f, unsigned intf, unsigned alt) {
    auto *cdev = f->config->cdev;
    auto in_desc = cdev->gadget->speed == USB_SPEED_HIGH ? &vendor_hs_in_ep : &vendor_fs_in_ep;
    auto out_desc = cdev->gadget->speed == USB_SPEED_HIGH ? &vendor_hs_out_ep : &vendor_fs_out_ep;
    if (endpoints_enabled) {
        if (ep_out && ep_out->ops && ep_out->ops->set_halt) ep_out->ops->set_halt(ep_out, 1);
        if (ep_in && ep_in->ops && ep_in->ops->set_halt) ep_in->ops->set_halt(ep_in, 1);
        usb_ep_disable(ep_in); usb_ep_disable(ep_out);
        if (ep_out && ep_out->ops && ep_out->ops->set_halt) ep_out->ops->set_halt(ep_out, 0);
        if (ep_in && ep_in->ops && ep_in->ops->set_halt) ep_in->ops->set_halt(ep_in, 0);
        endpoints_enabled = false; read_pending = false;
    }
    int ret = usb_ep_enable(ep_out, out_desc);
    if (ret) return ret;
    ret = usb_ep_enable(ep_in, in_desc);
    if (ret) { usb_ep_disable(ep_out); return ret; }
    endpoints_enabled = true;
    device_connected = true;
    usb_vendor_read(cmd_buf, 512);
    return 0;
}

static void vendor_disable(struct usb_function *f) {
    (void)f;
    if (ep_out && ep_out->ops && ep_out->ops->set_halt) ep_out->ops->set_halt(ep_out, 1);
    usb_ep_disable(ep_in); usb_ep_disable(ep_out);
    endpoints_enabled = false; device_connected = false; read_pending = false;
}

static int vendor_config_bind(struct usb_configuration *c) { return usb_add_function(c, &vendor_f); }
static int vendor_composite_bind(struct usb_composite_dev *cdev) { return usb_add_config(cdev, &vendor_config); }

static void init_vendor_f(void) {
    vendor_f.name = "vendor_custom"; vendor_f.strings = nullptr;
    vendor_f.descriptors = vendor_fs_descs; vendor_f.hs_descriptors = vendor_hs_descs;
    vendor_f.ss_descriptors = nullptr; vendor_f.config = nullptr;
    vendor_f.bind = vendor_bind; vendor_f.unbind = nullptr;
    vendor_f.set_alt = vendor_set_alt; vendor_f.get_alt = nullptr;
    vendor_f.disable = vendor_disable; vendor_f.setup = nullptr;
    vendor_f.suspend = nullptr; vendor_f.resume = nullptr;
}
static void init_vendor_config(void) {
    vendor_config.label = "USB-SD"; vendor_config.strings = nullptr;
    vendor_config.descriptors = nullptr; vendor_config.bind = vendor_config_bind;
    vendor_config.unbind = nullptr; vendor_config.setup = nullptr;
    vendor_config.bConfigurationValue = 1; vendor_config.iConfiguration = 0;
    vendor_config.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER;
    vendor_config.bMaxPower = 0x32;
}
static void init_vendor_driver(void) {
    vendor_driver.name = "vendor_custom_driver"; vendor_driver.dev = &device_desc;
    vendor_driver.strings = stringtab; vendor_driver.bind = vendor_composite_bind;
    vendor_driver.unbind = nullptr; vendor_driver.disconnect = nullptr;
    vendor_driver.suspend = nullptr; vendor_driver.resume = nullptr;
}

int usb_gadget_init(void) {
    init_vendor_f(); init_vendor_config(); init_vendor_driver();
    return usb_composite_register(&vendor_driver);
}

static void send_ack(uint32_t count) {
    ack_buf[0] = CMD_BIG;
    ack_buf[1] = (char)(count >> 24);
    ack_buf[2] = (char)(count >> 16);
    ack_buf[3] = (char)(count >> 8);
    ack_buf[4] = (char)(count);
    ack_buf[5] = 'O'; ack_buf[6] = 'K';
    auto *req = usb_ep_alloc_request(ep_in, GFP_ATOMIC);
    if (!req) return;
    req->buf = ack_buf; req->length = 7;
    req->complete = [](struct usb_ep *ep, struct usb_request *r) {
        usb_ep_free_request(ep, r);
        uart_puts("[USB] ACK sent\n");
    };
    req->no_interrupt = 0; req->zero = 0;
    usb_ep_queue(ep_in, req, GFP_ATOMIC);
}

void usb_vendor_read(void *buf, int len) {
    if (!ep_out || !buf || len <= 0 || !device_connected || !endpoints_enabled) return;
    if (read_pending) return;
    auto *req = usb_ep_alloc_request(ep_out, GFP_ATOMIC);
    if (!req) return;
    req->buf = buf; req->length = (unsigned)len;
    req->complete = [](struct usb_ep *ep, struct usb_request *r) {
        read_pending = false;
        if (r->status != 0 || r->actual == 0) {
            usb_ep_free_request(ep, r);
            usb_vendor_read(cmd_buf, 512);
            return;
        }
        uint32_t n = r->actual;
        uint8_t *d = (uint8_t*)r->buf;

        // Header packet: [CMD_BIG, size_be32...]
        if (!xfer_active && n >= 5 && d[0] == CMD_BIG) {
            xfer_total = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) |
                         ((uint32_t)d[3] << 8) | d[4];
            if (xfer_total > g_file_max) xfer_total = g_file_max;
            g_file_len = 0; g_file_ready = 0;
            xfer_active = 1;
            uart_puts("[USB] Receiving "); uart_puts(hex32(xfer_total)); uart_puts(" bytes\n");
            // Next reads go to data_buf
            usb_ep_free_request(ep, r);
            usb_vendor_read(data_buf, sizeof(data_buf));
            return;
        }
        // CMD_ECHO: --write mode
        else if (!xfer_active && n > 1 && d[0] == CMD_ECHO) {
            for (uint32_t i = 1; i < n; i++) {
                char c = (char)d[i]; if (c == '\n') uart_putc('\r'); uart_putc(c);
            }
        }
        // File data — store, print AND echo back to host
        else if (xfer_active) {
            uint32_t remain = xfer_total - g_file_len;
            if (n > remain) n = remain;
            memcpy(g_file_buf + g_file_len, d, n);
            for (uint32_t i = 0; i < n; i++) uart_putc((char)d[i]);
            g_file_len += n;

            // Echo data back on IN endpoint (dedicated echo_buf for DMA safety)
            memcpy(echo_buf, d, n);
            auto *ereq = usb_ep_alloc_request(ep_in, GFP_ATOMIC);
            if (ereq) {
                ereq->buf = echo_buf; ereq->length = n;
                ereq->complete = [](struct usb_ep *ep, struct usb_request *r) {
                    uart_puts(".");
                    usb_ep_free_request(ep, r);
                };
                ereq->no_interrupt = 0; ereq->zero = 0;
                int eret = usb_ep_queue(ep_in, ereq, GFP_ATOMIC);
                if (eret) { uart_puts("[USB] echo QUEUE FAIL\n"); usb_ep_free_request(ep_in, ereq); }
                else uart_puts("[USB] echo QUEUED\n");
            } else {
                uart_puts("[USB] echo alloc FAIL\n");
            }

            usb_ep_free_request(ep, r);
            if (g_file_len >= xfer_total) {
                xfer_active = 0;
                g_file_ready = 1;
                send_ack(g_file_len);
                uart_puts("\n[USB] Transfer complete\n");
            } else {
                usb_vendor_read(data_buf, sizeof(data_buf));
            }
            return;
        }

        usb_ep_free_request(ep, r);
        usb_vendor_read(cmd_buf, 512);
    };
    req->no_interrupt = 0; req->zero = 0;
    usb_ep_queue(ep_out, req, GFP_ATOMIC);
    read_pending = true;
}
