#include <stdint.h>
#include "uboot_compat.hpp"
#include "bit_field.hpp"

extern "C" void uart_puts(const char* s);
extern "C" void uart_putc(char c);
extern "C" char* hex32(uint32_t val);

bool device_connected = false;
bool endpoints_enabled = false;
bool read_pending = false;

void usb_vendor_set_connected(int connected);
void usb_vendor_start_read_queue(void);
int usb_vendor_read(void *buf, int len);
int usb_vendor_write(const void *buf, int len);

namespace {

static constexpr uint8_t CMD_ECHO = 0x01;
static constexpr uint8_t CMD_BIG_TRANSFER = 0x02;
static constexpr uint8_t CMD_BRIDGE = 0x03;
static constexpr uint8_t CMD_LOG = 0x04;

static constexpr uint32_t RING_BUF_SIZE = 262144;
static constexpr uint32_t BIG_XFER_CHUNK = 65536;
static constexpr uint32_t USB_DATA_BUF_SIZE = BIG_XFER_CHUNK;
static constexpr uint32_t CMD_READ_SIZE = 512;

struct vendor_data {
    struct usb_ep *ep_in;
    struct usb_ep *ep_out;
};

static struct vendor_data vdata;

static char usb_data_buffer[USB_DATA_BUF_SIZE] __attribute__((aligned(64), section(".dma_nc")));
static char ring_buffer[RING_BUF_SIZE] __attribute__((aligned(64), section(".dma_nc")));

static uint32_t ring_head = 0;
static uint32_t ring_tail = 0;
static uint32_t ring_count = 0;
static uint32_t big_total_expected = 0;
static uint32_t big_rx_bytes = 0;
static uint32_t big_tx_bytes = 0;
static int big_transfer_active = 0;

static constexpr int BIG_TX_IDLE = 0;
static constexpr int BIG_TX_DATA = 1;
static constexpr int BIG_TX_ACK  = 2;
static int      big_tx_state = BIG_TX_IDLE;
static int      uart_sink = 0;
static uint32_t big_tx_chunk = 0;
static char     big_ack_buf[7] __attribute__((aligned(64), section(".dma_nc")));

static struct usb_interface_descriptor vendor_intf = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
};

static struct usb_endpoint_descriptor vendor_fs_in_ep __attribute__((aligned(2))) = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(64),
};

static struct usb_endpoint_descriptor vendor_fs_out_ep __attribute__((aligned(2))) = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(64),
};

static struct usb_endpoint_descriptor vendor_hs_in_ep __attribute__((aligned(2))) = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor vendor_hs_out_ep __attribute__((aligned(2))) = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_descriptor_header *vendor_fs_descs[] = {
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_intf),
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_fs_in_ep),
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_fs_out_ep),
    nullptr,
};

static struct usb_descriptor_header *vendor_hs_descs[] = {
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_intf),
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_hs_in_ep),
    reinterpret_cast<struct usb_descriptor_header*>(&vendor_hs_out_ep),
    nullptr,
};

static struct usb_device_descriptor device_desc = {
    .bLength = sizeof(device_desc),
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = cpu_to_le16(0x0200),
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .idVendor = cpu_to_le16(0x2207),
    .idProduct = cpu_to_le16(0x0019),
    .bcdDevice = cpu_to_le16(0x0100),
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static struct usb_string strings[] = {
    { 1, "Luckfox" },
    { 2, "Lyra RK3506 USB UART Logger" },
    { 3, "12345678" },
    { },
};

static struct usb_gadget_strings stringtab_lang = {
    .language = 0x0409,
    .strings = strings,
};

static struct usb_gadget_strings *stringtab[] = {
    &stringtab_lang,
    nullptr,
};

} // anonymous namespace

static void big_reset_state(void);
static void big_tx_pump(void);
static void big_data_chunk_done(void);

static int vendor_bind(struct usb_configuration *c, struct usb_function *f) {
    struct usb_composite_dev *cdev = c->cdev;
    int id = usb_interface_id(c, f);
    if (id < 0) return id;
    vendor_intf.bInterfaceNumber = static_cast<uint8_t>(id);
    vdata.ep_in = usb_ep_autoconfig(cdev->gadget, &vendor_fs_in_ep);
    if (!vdata.ep_in) return -ENODEV;
    vdata.ep_in->driver_data = f;
    vdata.ep_out = usb_ep_autoconfig(cdev->gadget, &vendor_fs_out_ep);
    if (!vdata.ep_out) return -ENODEV;
    vdata.ep_out->driver_data = f;
    vendor_hs_in_ep.bEndpointAddress = vendor_fs_in_ep.bEndpointAddress;
    vendor_hs_out_ep.bEndpointAddress = vendor_fs_out_ep.bEndpointAddress;
    return 0;
}

static int vendor_set_alt(struct usb_function *f, unsigned intf, unsigned alt) {
    struct usb_composite_dev *cdev = f->config->cdev;
    const struct usb_endpoint_descriptor *in_desc, *out_desc;
    if (cdev->gadget->speed == USB_SPEED_HIGH) {
        in_desc = &vendor_hs_in_ep;
        out_desc = &vendor_hs_out_ep;
    } else {
        in_desc = &vendor_fs_in_ep;
        out_desc = &vendor_fs_out_ep;
    }
    if (endpoints_enabled) {
        if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt)
            vdata.ep_out->ops->set_halt(vdata.ep_out, 1);
        if (vdata.ep_in && vdata.ep_in->ops && vdata.ep_in->ops->set_halt)
            vdata.ep_in->ops->set_halt(vdata.ep_in, 1);
        usb_ep_disable(vdata.ep_in);
        usb_ep_disable(vdata.ep_out);
        if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt)
            vdata.ep_out->ops->set_halt(vdata.ep_out, 0);
        if (vdata.ep_in && vdata.ep_in->ops && vdata.ep_in->ops->set_halt)
            vdata.ep_in->ops->set_halt(vdata.ep_in, 0);
        endpoints_enabled = false;
        read_pending = false;
    }
    int ret = usb_ep_enable(vdata.ep_out, out_desc);
    if (ret) return ret;
    ret = usb_ep_enable(vdata.ep_in, in_desc);
    if (ret) { usb_ep_disable(vdata.ep_out); return ret; }
    endpoints_enabled = true;
    usb_vendor_set_connected(1);
    usb_vendor_start_read_queue();
    return 0;
}

static void vendor_disable(struct usb_function *f) {
    (void)f;
    if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt)
        vdata.ep_out->ops->set_halt(vdata.ep_out, 1);
    usb_ep_disable(vdata.ep_in);
    usb_ep_disable(vdata.ep_out);
    memset(usb_data_buffer, 0, sizeof(usb_data_buffer));
    endpoints_enabled = false;
    device_connected = false;
    read_pending = false;
}

static struct usb_function vendor_f;
static struct usb_configuration vendor_config;
static struct usb_composite_driver vendor_driver;

static int vendor_config_bind(struct usb_configuration *c) {
    return usb_add_function(c, &vendor_f);
}

static int vendor_composite_bind(struct usb_composite_dev *cdev) {
    return usb_add_config(cdev, &vendor_config);
}

static void vendor_ep_in_complete(struct usb_ep *ep, struct usb_request *req);
static void vendor_ep_out_complete(struct usb_ep *ep, struct usb_request *req);

static void init_vendor_f(void) {
    vendor_f.name = "vendor_custom";
    vendor_f.strings = nullptr;
    vendor_f.descriptors = vendor_fs_descs;
    vendor_f.hs_descriptors = vendor_hs_descs;
    vendor_f.ss_descriptors = nullptr;
    vendor_f.config = nullptr;
    vendor_f.bind = vendor_bind;
    vendor_f.unbind = nullptr;
    vendor_f.set_alt = vendor_set_alt;
    vendor_f.get_alt = nullptr;
    vendor_f.disable = vendor_disable;
    vendor_f.setup = nullptr;
    vendor_f.suspend = nullptr;
    vendor_f.resume = nullptr;
}

static void init_vendor_config(void) {
    vendor_config.label = "Vendor Custom Config";
    vendor_config.strings = nullptr;
    vendor_config.descriptors = nullptr;
    vendor_config.bind = vendor_config_bind;
    vendor_config.unbind = nullptr;
    vendor_config.setup = nullptr;
    vendor_config.bConfigurationValue = 1;
    vendor_config.iConfiguration = 0;
    vendor_config.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER;
    vendor_config.bMaxPower = 0x32;
}

static void init_vendor_driver(void) {
    vendor_driver.name = "vendor_custom_driver";
    vendor_driver.dev = &device_desc;
    vendor_driver.strings = stringtab;
    vendor_driver.bind = vendor_composite_bind;
    vendor_driver.unbind = nullptr;
    vendor_driver.disconnect = nullptr;
    vendor_driver.suspend = nullptr;
    vendor_driver.resume = nullptr;
}

int usb_gadget_init(void) {
    init_vendor_f();
    init_vendor_config();
    init_vendor_driver();
    return usb_composite_register(&vendor_driver);
}

int usb_vendor_write(const void *buf, int len) {
    struct usb_request *req;
    if (!vdata.ep_in || !buf || len <= 0) return -EINVAL;
    req = usb_ep_alloc_request(vdata.ep_in, GFP_ATOMIC);
    if (!req) return -ENOMEM;
    req->buf = const_cast<void*>(buf);
    req->length = static_cast<unsigned>(len);
    req->complete = vendor_ep_in_complete;
    req->no_interrupt = 0;
    req->zero = 0;
    int ret = usb_ep_queue(vdata.ep_in, req, GFP_ATOMIC);
    if (ret) { usb_ep_free_request(vdata.ep_in, req); return ret; }
    return len;
}

int usb_vendor_read(void *buf, int len) {
    struct usb_request *req;
    if (!vdata.ep_out || !buf || len <= 0) return -EINVAL;
    if (!device_connected || !endpoints_enabled) return -ENODEV;
    if (read_pending) return -EBUSY;
    req = usb_ep_alloc_request(vdata.ep_out, GFP_ATOMIC);
    if (!req) return -ENOMEM;
    req->buf = buf;
    req->length = static_cast<unsigned>(len);
    req->complete = vendor_ep_out_complete;
    req->no_interrupt = 0;
    req->zero = 0;
    int ret = usb_ep_queue(vdata.ep_out, req, GFP_ATOMIC);
    if (ret) { usb_ep_free_request(vdata.ep_out, req); return ret; }
    read_pending = true;
    return len;
}

void usb_vendor_set_connected(int connected) {
    device_connected = connected ? true : false;
    if (!connected) {
        read_pending = false;
        big_reset_state();
        memset(usb_data_buffer, 0, sizeof(usb_data_buffer));
    }
}

void usb_vendor_start_read_queue(void) {
    if (big_transfer_active) return;
    if (!read_pending && device_connected && endpoints_enabled && vdata.ep_out)
        usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
}

static void big_arm_rx(void) {
    uint32_t len = BIG_XFER_CHUNK;
    if (len > RING_BUF_SIZE - ring_head)
        len = RING_BUF_SIZE - ring_head;
    uint32_t remaining = big_total_expected - big_rx_bytes;
    uint32_t rem_aligned = (remaining + 511u) & ~511u;
    if (len > rem_aligned) len = rem_aligned;
    if (len == 0) return;
    usb_vendor_read(&ring_buffer[ring_head], static_cast<int>(len));
}

static void big_tx_pump(void) {
    if (!big_transfer_active || big_tx_state != BIG_TX_IDLE)
        return;

    if (ring_count > 0) {
        uint32_t chunk = ring_count;
        if (chunk > BIG_XFER_CHUNK) chunk = BIG_XFER_CHUNK;
        if (chunk > RING_BUF_SIZE - ring_tail)
            chunk = RING_BUF_SIZE - ring_tail;
        bool final_chunk = (big_rx_bytes >= big_total_expected) && (chunk == ring_count);
        if (!final_chunk) {
            chunk &= ~63u;
            if (chunk == 0) return;
        }
        big_tx_chunk = chunk;
        big_tx_state = BIG_TX_DATA;
        usb_vendor_write(&ring_buffer[ring_tail], static_cast<int>(chunk));
        return;
    }

    if (big_rx_bytes >= big_total_expected && big_tx_bytes >= big_total_expected) {
        big_ack_buf[0] = CMD_BIG_TRANSFER;
        big_ack_buf[1] = static_cast<char>((big_tx_bytes >> 24) & 0xFF);
        big_ack_buf[2] = static_cast<char>((big_tx_bytes >> 16) & 0xFF);
        big_ack_buf[3] = static_cast<char>((big_tx_bytes >> 8) & 0xFF);
        big_ack_buf[4] = static_cast<char>(big_tx_bytes & 0xFF);
        big_ack_buf[5] = 'O';
        big_ack_buf[6] = 'K';
        big_tx_state = BIG_TX_ACK;
        if (usb_vendor_write(big_ack_buf, sizeof(big_ack_buf)) < 0)
            big_tx_state = BIG_TX_IDLE;
    }
}

static void big_reset_state(void) {
    big_transfer_active = 0;
    uart_sink = 0;
    big_tx_state = BIG_TX_IDLE;
    big_tx_chunk = 0;
    ring_head = ring_tail = ring_count = 0;
    big_rx_bytes = big_tx_bytes = big_total_expected = 0;
}

static void big_data_chunk_done(void) {
    ring_tail = (ring_tail + big_tx_chunk) % RING_BUF_SIZE;
    ring_count -= big_tx_chunk;
    big_tx_bytes += big_tx_chunk;
    big_tx_chunk = 0;
    big_tx_state = BIG_TX_IDLE;
    big_tx_pump();
}

static void vendor_ep_in_complete(struct usb_ep *ep, struct usb_request *req) {
    usb_ep_free_request(ep, req);
    if (big_tx_state == BIG_TX_DATA) {
        big_data_chunk_done();
    } else if (big_tx_state == BIG_TX_ACK) {
        big_reset_state();
        uart_puts("[USB] Transfer complete, ACK sent\n");
        if (device_connected && endpoints_enabled && !read_pending)
            usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
    }
}

static void vendor_ep_out_complete(struct usb_ep *ep, struct usb_request *req) {
    if (req->status == 0) {
        if (big_transfer_active) {
            uint32_t n = req->actual;
            if (uart_sink) {
                char* d = static_cast<char*>(req->buf);
                for (uint32_t i = 0; i < n; i++)
                    uart_putc(d[i]);
            }
            ring_head = (ring_head + n) % RING_BUF_SIZE;
            ring_count += n;
            big_rx_bytes += n;
            read_pending = false;
            if (big_rx_bytes < big_total_expected) {
                if (device_connected && endpoints_enabled)
                    big_arm_rx();
            }
            big_tx_pump();
        } else if (req->actual > 1 && static_cast<char*>(req->buf)[0] == CMD_ECHO) {
            for (int i = 0; i < static_cast<int>(req->actual) - 1; i++) {
                char c = static_cast<char*>(req->buf)[i + 1];
                usb_data_buffer[i] = c;
                if (c == '\n') uart_putc('\r');
                uart_putc(c);
            }
            if (req->actual > 1) uart_putc('\n');
            usb_vendor_write(usb_data_buffer, static_cast<int>(req->actual) - 1);
            read_pending = false;
            if (device_connected && endpoints_enabled)
                usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
        } else if (req->actual >= 5 &&
                   (static_cast<char*>(req->buf)[0] == CMD_BIG_TRANSFER ||
                    static_cast<char*>(req->buf)[0] == CMD_BRIDGE ||
                    static_cast<char*>(req->buf)[0] == CMD_LOG)) {
            const uint8_t *b = static_cast<const uint8_t*>(req->buf);
            uint32_t total_size = (static_cast<uint32_t>(b[1]) << 24) |
                                  (static_cast<uint32_t>(b[2]) << 16) |
                                  (static_cast<uint32_t>(b[3]) << 8)  |
                                   static_cast<uint32_t>(b[4]);
            big_reset_state();
            big_total_expected = total_size;
            big_transfer_active = 1;
            uart_sink = 1;

            uart_puts("[USB] Receiving ");
            uart_puts(hex32(total_size));
            uart_puts(" bytes...\n");

            read_pending = false;
            if (big_rx_bytes < big_total_expected && device_connected && endpoints_enabled)
                big_arm_rx();
            big_tx_pump();
        } else {
            read_pending = false;
            if (device_connected && endpoints_enabled)
                usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
        }
    } else {
        read_pending = false;
        if (device_connected && endpoints_enabled)
            usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
    }
    usb_ep_free_request(ep, req);
}

/* ===== UART Sink API (called from LOG task) ===== */

extern "C" uint32_t uart_sink_avail(void) {
    return ring_count;
}

extern "C" uint32_t uart_sink_read(void* dst, uint32_t max) {
    uint32_t n = ring_count;
    if (n > max) n = max;
    if (n == 0) return 0;
    if (!uart_sink) return 0;
    uint32_t first = RING_BUF_SIZE - ring_tail;
    if (first > n) first = n;
    memcpy(dst, &ring_buffer[ring_tail], first);
    if (n > first)
        memcpy(static_cast<char*>(dst) + first, &ring_buffer[0], n - first);
    ring_tail = (ring_tail + n) % RING_BUF_SIZE;
    ring_count -= n;
    big_tx_bytes += n;
    return n;
}

extern "C" int uart_sink_finished(void) {
    return uart_sink && big_rx_bytes >= big_total_expected && ring_count == 0;
}

extern "C" void uart_sink_end(void) {
    big_ack_buf[0] = CMD_BIG_TRANSFER;
    big_ack_buf[1] = static_cast<char>((big_tx_bytes >> 24) & 0xFF);
    big_ack_buf[2] = static_cast<char>((big_tx_bytes >> 16) & 0xFF);
    big_ack_buf[3] = static_cast<char>((big_tx_bytes >> 8) & 0xFF);
    big_ack_buf[4] = static_cast<char>(big_tx_bytes & 0xFF);
    big_ack_buf[5] = 'O';
    big_ack_buf[6] = 'K';
    big_tx_state = BIG_TX_ACK;
    if (usb_vendor_write(big_ack_buf, sizeof(big_ack_buf)) < 0) {
        big_tx_state = BIG_TX_IDLE;
        uart_puts("[USB] ACK send failed\n");
        big_reset_state();
        if (device_connected && endpoints_enabled && !read_pending)
            usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
    }
}
