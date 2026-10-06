/**
 * @file usb_vendor_gadget.cpp
 * @brief Custom vendor-specific USB gadget implementation.
 *
 * This file implements a vendor-specific USB function and composite driver.
 * It provides a simple command-response interface (ECHO, BIG_TRANSFER) 
 * using bulk endpoints.
 */

#include <stdint.h>

#include "uboot_compat.hpp"
#include "bit_field.hpp"

bool device_connected = false;
bool endpoints_enabled = false;
bool read_pending = false;

void usb_vendor_set_connected(int connected);
void usb_vendor_start_read_queue(void);
int usb_vendor_read(void *buf, int len);
int usb_vendor_write(const void *buf, int len);

/* ---- Phase-3 station hooks (weak: echo-only builds link unchanged) ----
 * station_command: first look at an unrecognized command packet; returns
 * nonzero if the station consumed it. station_tx_complete: an IN request
 * finished that was neither a big-transfer chunk nor the big ACK. */
/// Weak station hook: inspect an unrecognized command packet. @param pkt Packet. @param len Length. @return non-zero if consumed.
extern "C" int  station_command(const uint8_t* pkt, uint32_t len) __attribute__((weak));
/// Weak station hook: an IN request finished that was neither a big-transfer chunk nor the big ACK.
extern "C" void station_tx_complete(void) __attribute__((weak));

namespace {

static constexpr uint8_t CMD_ECHO = 0x01;
static constexpr uint8_t CMD_BIG_TRANSFER = 0x02;
static constexpr uint8_t CMD_BRIDGE = 0x03;   // usb0-1 build: forward to USB1/PC1

// Per-transfer logging floods the UART during big transfers (and at 115200
// baud it throttles the data path). Verbose logs only in debug builds
// (make DEBUG=1 -> -DUSB_DEBUG); release builds print a progress dot every
// 64 packets instead. Errors and one-time events always print.
/// Verbose per-transfer debug print (no-op unless built with -DUSB_DEBUG). @param x String literal to emit.
#ifdef USB_DEBUG
#define VDBG(x) uart_puts(x)
#else
#define VDBG(x) ((void)0)
#endif

// 1 MB FIFO: decouples RX from TX so reception rarely stalls waiting for
// the echo to drain (the RK3506 has ~160 MB of DDR; the linker BSS guard
// was raised accordingly).
static constexpr uint32_t RING_BUF_SIZE = 1048576;
// Multi-packet DMA: during a big transfer, OUT reads and IN chunks span up
// to 128 bulk packets per DMA program (DOEPTSIZ/DIEPTSIZ carry PKTCNT and a
// 19-bit XFERSIZE, max ~512 KB), so one interrupt covers a whole 64 KB
// instead of one per 512-byte packet. Command reads outside a big transfer
// stay one packet so a lone command completes immediately.
#ifdef USB_FORCE_FS
// FS bulk MPS is 64: a 64 KB chunk would need 1024 packets, exceeding the
// 10-bit PKTCNT field (max 1023). 32 KB = 512 packets.
static constexpr uint32_t BIG_XFER_CHUNK = 32768;
#else
static constexpr uint32_t BIG_XFER_CHUNK = 65536;
#endif
static constexpr uint32_t USB_DATA_BUF_SIZE = BIG_XFER_CHUNK;
static constexpr uint32_t CMD_READ_SIZE = 512;

struct vendor_data {
    struct usb_ep *ep_in;
    struct usb_ep *ep_out;
};

static struct vendor_data vdata;

/* DMA targets: must live in the Normal-NC .dma_nc window (zero-copy). */
static char usb_data_buffer[USB_DATA_BUF_SIZE] __attribute__((aligned(64), section(".dma_nc")));

static char ring_buffer[RING_BUF_SIZE] __attribute__((aligned(64), section(".dma_nc")));
// Streaming big transfer: ring_buffer is a circular FIFO between the OUT
// (RX) and IN (TX) endpoints, so the total transfer size is unbounded --
// data is echoed back while the rest is still arriving. The DWC2 driver
// holds ONE pending request per endpoint direction, so exactly one TX
// chunk is in flight at a time (chained from its completion), and RX is
// re-armed only while the FIFO has room for a full 512-byte packet
// (otherwise the host simply sees NAKs until TX drains the ring).
static uint32_t ring_head = 0;         // write index (RX side)
static uint32_t ring_tail = 0;         // read index (TX side)
static uint32_t ring_count = 0;        // bytes currently buffered
static uint32_t big_total_expected = 0;
static uint32_t big_rx_bytes = 0;      // received from host so far
static uint32_t big_tx_bytes = 0;      // echoed back so far
static int big_transfer_active = 0;
static int big_rx_stalled = 0;         // RX paused waiting for FIFO space

static constexpr int BIG_TX_IDLE = 0;
static constexpr int BIG_TX_DATA = 1;  // data chunk in flight
static constexpr int BIG_TX_ACK  = 2;  // final ACK packet in flight
static constexpr int BIG_TX_HDR  = 3;  // bridge: 512-byte header to PC1 in flight
static int      big_tx_state = BIG_TX_IDLE;
// Phase-3 station: RX streams into the ring but is CONSUMED by the FS path
// instead of echoed (no big_tx_pump, no automatic ACK).
static int      station_sink = 0;
static uint32_t big_tx_chunk = 0;      // size of the chunk in flight
static char     big_ack_buf[7] __attribute__((aligned(64), section(".dma_nc")));

#ifdef USB_BRIDGE
// Bridge mode: data received on USB0 (from PC0) is pumped out of the ring
// through USB1's bulk IN endpoint (to PC1) instead of echoed. USB0
// completions run in IRQ context while USB1 is serviced from the main
// loop, so main-loop entry points mask IRQs around shared-state updates.
#ifdef USB_FORCE_FS
static constexpr uint32_t BRIDGE_EP_MPS = 64;
#else
static constexpr uint32_t BRIDGE_EP_MPS = 512;
#endif
static int  big_bridge = 0;            // this transfer forwards to USB1
static int  bridge_hdr_sent = 0;
static char bridge_hdr_buf[512] __attribute__((aligned(64), section(".dma_nc")));

extern "C" {
    int usb1_is_configured(void);
    int usb1_tx(const void *buf, int len);
}

static inline uint32_t irq_save(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    return cpsr;
}
static inline void irq_restore(uint32_t cpsr) {
    if (!(cpsr & 0x80U))
        __asm__ volatile("cpsie i" ::: "memory");
}
#endif

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
    { 2, "Lyra RK3506 USB" },
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

/// UDC callback: allocate the interface ID and bulk endpoints. @param c Configuration. @param f Vendor function. @return 0 on success, <0 on error.
static int vendor_bind(struct usb_configuration *c, struct usb_function *f) {
    struct usb_composite_dev *cdev = c->cdev;
    int id;

    id = usb_interface_id(c, f);
    if (id < 0)
        return id;
    vendor_intf.bInterfaceNumber = static_cast<uint8_t>(id);

    vdata.ep_in = usb_ep_autoconfig(cdev->gadget, &vendor_fs_in_ep);
    if (!vdata.ep_in)
        return -ENODEV;
    vdata.ep_in->driver_data = f;

    vdata.ep_out = usb_ep_autoconfig(cdev->gadget, &vendor_fs_out_ep);
    if (!vdata.ep_out)
        return -ENODEV;
    vdata.ep_out->driver_data = f;

    vendor_hs_in_ep.bEndpointAddress = vendor_fs_in_ep.bEndpointAddress;
    vendor_hs_out_ep.bEndpointAddress = vendor_fs_out_ep.bEndpointAddress;

    uart_puts("VENDOR: autoconfig IN addr=0x");
    uart_puts(hex32(vendor_fs_in_ep.bEndpointAddress));
    uart_puts(" OUT addr=0x");
    uart_puts(hex32(vendor_fs_out_ep.bEndpointAddress));
    uart_puts("\n");

    return 0;
}

/// UDC callback: enable the endpoints for the selected setting. @param f Function. @param intf Interface. @param alt Alternate setting. @return 0 on success.
static int vendor_set_alt(struct usb_function *f, unsigned intf, unsigned alt) {
    struct usb_composite_dev *cdev = f->config->cdev;
    const struct usb_endpoint_descriptor *in_desc, *out_desc;
    int ret;

    if (cdev->gadget->speed == USB_SPEED_HIGH) {
        in_desc = &vendor_hs_in_ep;
        out_desc = &vendor_hs_out_ep;
    } else {
        in_desc = &vendor_fs_in_ep;
        out_desc = &vendor_fs_out_ep;
    }

    if (endpoints_enabled) {
        uart_puts("VENDOR: Cleanup before re-enable\n");

        if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt) {
            vdata.ep_out->ops->set_halt(vdata.ep_out, 1);
        }
        if (vdata.ep_in && vdata.ep_in->ops && vdata.ep_in->ops->set_halt) {
            vdata.ep_in->ops->set_halt(vdata.ep_in, 1);
        }

        usb_ep_disable(vdata.ep_in);
        usb_ep_disable(vdata.ep_out);

        if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt) {
            vdata.ep_out->ops->set_halt(vdata.ep_out, 0);
        }
        if (vdata.ep_in && vdata.ep_in->ops && vdata.ep_in->ops->set_halt) {
            vdata.ep_in->ops->set_halt(vdata.ep_in, 0);
        }

        endpoints_enabled = false;
        read_pending = false;

        uart_puts("VENDOR: Cleanup complete\n");
    }

    ret = usb_ep_enable(vdata.ep_out, out_desc);
    if (ret) {
        uart_puts("VENDOR: EP-OUT enable FAILED\n");
        return ret;
    }

    uart_puts("VENDOR: EP-OUT enabled, desc=");
    uart_puts(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->desc)));
    uart_puts(" ep=");
    uart_puts(hex32(reinterpret_cast<uint32_t>(vdata.ep_out)));
    uart_puts(" ops=");
    uart_puts(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->ops)));
    uart_puts(" ops->enable=");
    uart_puts(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->ops->enable)));
    uart_puts("\n");

    ret = usb_ep_enable(vdata.ep_in, in_desc);
    if (ret) {
        uart_puts("VENDOR: EP-IN enable FAILED\n");
        usb_ep_disable(vdata.ep_out);
        return ret;
    }

    uart_puts("VENDOR: EP-IN and EP-OUT enabled\n");

    endpoints_enabled = true;
    usb_vendor_set_connected(1);
    usb_vendor_start_read_queue();

    return 0;
}

/// UDC callback: halt and disable both bulk endpoints. @param f The vendor function.
static void vendor_disable(struct usb_function *f) {
    (void)f;
    uart_puts("VENDOR: Halting and clearing both endpoints\n");

    if (vdata.ep_out && vdata.ep_out->ops && vdata.ep_out->ops->set_halt)
        vdata.ep_out->ops->set_halt(vdata.ep_out, 1);

    usb_ep_disable(vdata.ep_in);
    usb_ep_disable(vdata.ep_out);

    memset(usb_data_buffer, 0, sizeof(usb_data_buffer));

    endpoints_enabled = false;
    device_connected = false;
    read_pending = false;

    uart_puts("VENDOR: EP-IN and EP-OUT disabled\n");
}

static struct usb_function vendor_f;              ///< The single vendor bulk function.
static struct usb_configuration vendor_config;    ///< The single configuration holding @ref vendor_f.
static struct usb_composite_driver vendor_driver; ///< The composite driver registered with the UDC.

/// UDC callback: add @ref vendor_f to configuration @p c. @param c Configuration. @return 0 on success.
static int vendor_config_bind(struct usb_configuration *c) {
    uart_puts("VENDOR: vendor_config_bind called\n");
    int ret = usb_add_function(c, &vendor_f);
    uart_puts("VENDOR: vendor_config_bind done\n");
    return ret;
}

/// UDC callback: add @ref vendor_config to the device. @param cdev Composite device. @return 0 on success.
static int vendor_composite_bind(struct usb_composite_dev *cdev) {
    int ret = usb_add_config(cdev, &vendor_config);
    if (ret)
        return ret;
    return 0;
}

static void vendor_ep_in_complete(struct usb_ep *ep, struct usb_request *req);
static void vendor_ep_out_complete(struct usb_ep *ep, struct usb_request *req);

/// Populate @ref vendor_f (descriptors + callbacks) before registration.
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

/// Populate @ref vendor_config before registration.
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

/// Populate @ref vendor_driver before registration.
static void init_vendor_driver(void) {
    vendor_driver.name = "vendor_custom_driver";
    vendor_driver.dev = &device_desc;
    vendor_driver.strings = stringtab;
    vendor_driver.bind = vendor_composite_bind;
    vendor_driver.unbind = nullptr;
    vendor_driver.disconnect = nullptr;
    vendor_driver.suspend = nullptr;
    vendor_driver.resume = nullptr;
    uart_puts("VENDOR: driver init dev=");
    uart_puts(hex32(reinterpret_cast<uint32_t>(vendor_driver.dev)));
    uart_puts("\n");
}

/**
 * @brief Initializes the vendor gadget and registers the composite driver.
 * @return 0 on success, or negative error code.
 */
int usb_gadget_init(void) {
    int ret;

    init_vendor_f();
    init_vendor_config();
    init_vendor_driver();

    uart_puts("GADGET: Calling usb_composite_register\n");
    ret = usb_composite_register(&vendor_driver);
    if (ret) {
        uart_puts("GADGET: usb_composite_register FAILED\n");
    } else {
        uart_puts("GADGET: usb_composite_register OK\n");
    }
    return ret;
}

/// DWC2 DCFG register address (device address field), for deferred SET_ADDRESS.
static constexpr uintptr_t DCFG_ADDR = 0xFFB00000U + 0xC700U;

static uint8_t pending_address = 0;   ///< USB address staged by SET_ADDRESS, applied after the status stage.

/// Write @ref pending_address into the DWC2 DCFG once the status stage completes.
[[maybe_unused]] static void apply_pending_address(void) {
    if (pending_address != 0) {
        rk3506::BitField<uint32_t> dcfg{reinterpret_cast<volatile uint32_t*>(DCFG_ADDR)};
        dcfg.set_field(0x7F00U, static_cast<uint32_t>(pending_address), 8);
        pending_address = 0;
    }
}

/**
 * @brief Queues a data buffer for transmission over the vendor IN endpoint.
 * @param buf Pointer to the data to write.
 * @param len Length of the data in bytes.
 * @return Number of bytes queued, or negative error code.
 */
int usb_vendor_write(const void *buf, int len) {
    struct usb_request *req;
    int ret;

    if (!vdata.ep_in || !buf || len <= 0)
        return -EINVAL;

    VDBG("VENDOR: write EP-IN len=");
    VDBG(hex32(static_cast<uint32_t>(len)));
    VDBG("\n");

    req = usb_ep_alloc_request(vdata.ep_in, GFP_ATOMIC);
    if (!req) {
        uart_puts("VENDOR: alloc_request failed for EP-IN\n");
        return -ENOMEM;
    }

    req->buf = const_cast<void*>(buf);
    req->length = static_cast<unsigned>(len);
    req->complete = vendor_ep_in_complete;
    req->no_interrupt = 0;
    req->zero = 0;

    ret = usb_ep_queue(vdata.ep_in, req, GFP_ATOMIC);
    if (ret) {
        uart_puts("VENDOR: EP-IN queue failed\n");
        usb_ep_free_request(vdata.ep_in, req);
        return ret;
    }

    return len;
}

/**
 * @brief Queues a request to read data from the vendor OUT endpoint.
 * @param buf Pointer to the destination buffer.
 * @param len Maximum length to read.
 * @return Length of the queue request, or negative error code.
 */
int usb_vendor_read(void *buf, int len) {
    struct usb_request *req;
    int ret;

    if (!vdata.ep_out || !buf || len <= 0)
        return -EINVAL;

    if (!device_connected || !endpoints_enabled) {
        uart_puts("VENDOR: read fail - device not ready (connected=");
        uart_puts(hex32(device_connected ? 1u : 0u));
        uart_puts(" enabled=");
        uart_puts(hex32(endpoints_enabled ? 1u : 0u));
        uart_puts(")\n");
        return -ENODEV;
    }

    if (read_pending) {
        uart_puts("VENDOR: read fail - already pending\n");
        return -EBUSY;
    }

    VDBG("VENDOR: read EP-OUT len=");
    VDBG(hex32(static_cast<uint32_t>(len)));
    VDBG(" ops=");
    VDBG(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->ops)));
    VDBG(" ep=");
    VDBG(hex32(reinterpret_cast<uint32_t>(vdata.ep_out)));
    VDBG(" desc=");
    VDBG(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->desc)));
    VDBG("\n");

    if (!vdata.ep_out->desc) {
        uart_puts("VENDOR: EP-OUT descriptor is NULL!\n");
        return -ENODEV;
    }
    if (!vdata.ep_out->ops) {
        uart_puts("VENDOR: EP-OUT operations are NULL!\n");
        return -ENODEV;
    }
    if (!vdata.ep_out->ops->queue) {
        uart_puts("VENDOR: EP-OUT queue operation is NULL!\n");
        return -ENODEV;
    }

    req = usb_ep_alloc_request(vdata.ep_out, GFP_ATOMIC);
    if (!req) {
        uart_puts("VENDOR: alloc_request failed for EP-OUT\n");
        return -ENOMEM;
    }

    req->buf = buf;
    req->length = static_cast<unsigned>(len);
    req->complete = vendor_ep_out_complete;
    req->no_interrupt = 0;
    req->zero = 0;

    invalidate_dcache_range(reinterpret_cast<uintptr_t>(buf), reinterpret_cast<uintptr_t>(buf) + static_cast<uint32_t>(len));

    VDBG("VENDOR: About to queue EP-OUT...");
    VDBG(" req->buf=");
    VDBG(hex32(reinterpret_cast<uint32_t>(req->buf)));
    VDBG(" req->complete=");
    VDBG(hex32(reinterpret_cast<uint32_t>(req->complete)));
    VDBG(" ops->queue=");
    VDBG(hex32(reinterpret_cast<uint32_t>(vdata.ep_out->ops->queue)));
    VDBG("\n");

    ret = usb_ep_queue(vdata.ep_out, req, GFP_ATOMIC);
    if (ret) {
        uart_puts("VENDOR: EP-OUT queue failed ret=");
        uart_puts(hex32(static_cast<uint32_t>(ret)));
        uart_puts(" ep=");
        uart_puts(hex32(reinterpret_cast<uint32_t>(vdata.ep_out)));
        uart_puts(" req=");
        uart_puts(hex32(reinterpret_cast<uint32_t>(req)));
        uart_puts("\n");
        usb_ep_free_request(vdata.ep_out, req);
        return ret;
    }
    VDBG("VENDOR: EP-OUT queued OK\n");

    read_pending = true;
    return len;
}

/// Set the cached link-connected state (called from the gadget event path). @param connected Non-zero if enumerated.
void usb_vendor_set_connected(int connected) {
    device_connected = connected ? true : false;

    if (connected) {
        uart_puts("VENDOR: Device connected to host\n");
    } else {
        uart_puts("VENDOR: Device disconnected from host\n");
        read_pending = false;
        big_reset_state();
        memset(usb_data_buffer, 0, sizeof(usb_data_buffer));
    }
}

#ifdef USB_BRIDGE
extern "C" void bridge_usb1_tx_done(void) {
    uint32_t f = irq_save();
    if (big_tx_state == BIG_TX_HDR) {
        bridge_hdr_sent = 1;
        big_tx_state = BIG_TX_IDLE;
        big_tx_pump();
    } else if (big_tx_state == BIG_TX_DATA) {
        big_data_chunk_done();
    }
    irq_restore(f);
}

extern "C" void bridge_pump(void) {
    uint32_t f = irq_save();
    if (big_transfer_active)
        big_tx_pump();
    irq_restore(f);
}
#endif

void usb_vendor_start_read_queue(void) {
    int ret;
    // NEVER arm a command read while a big/station transfer owns the OUT
    // endpoint: during a ring stall read_pending is false with the stream
    // mid-flight, and a 512-byte command read here swallows payload and
    // credits the ring with data DMA never wrote (board-found via CRC
    // forensics: 512-byte stream drops + stale ring-lap blocks). Latent in
    // the original echo firmware too -- its stalls were just too short to
    // hit. The big machinery re-arms itself from its own completions.
    if (big_transfer_active)
        return;
    if (!read_pending && device_connected && endpoints_enabled && vdata.ep_out) {
        VDBG("VENDOR: Starting read queue\n");
        ret = usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
        if (ret < 0) {
            uart_puts("VENDOR: Initial read queue failed\n");
        }
    }
}

// Arm a zero-copy RX: DMA lands directly in the ring at ring_head, so the
// CPU never touches the payload (critical with the D-cache off — a bounce
// copy over uncached DDR caps the whole pipe at a few MB/s). Mid-transfer
// ring_head stays a 64 KB multiple (chunk size divides the ring size and
// only the final read may be short), which keeps the DOEPDMA address
// aligned and the chunk contiguous to the ring end.
/// Arm the next OUT transfer for a big streaming receive.
static void big_arm_rx(void) {
    uint32_t len = BIG_XFER_CHUNK;
    if (len > RING_BUF_SIZE - ring_head)
        len = RING_BUF_SIZE - ring_head;
    // Cap at the bytes still expected: a payload that is an exact multiple
    // of the packet size never sends a short packet, so a read armed past
    // the end of the stream would never complete (board-found with 512-byte
    // station PUTs; the echo path never hit it because ad-hoc test sizes
    // always ended in a short packet).
    // DWC2 OUT XFERSIZE must be an MPS multiple: round the cap UP to 512
    // (a 511-byte arm hangs the controller — board-found). The short final
    // packet still terminates the transfer with the true byte count.
    uint32_t remaining = big_total_expected - big_rx_bytes;
    uint32_t rem_aligned = (remaining + 511u) & ~511u;
    if (len > rem_aligned)
        len = rem_aligned;
    if (len == 0)
        return;
    usb_vendor_read(&ring_buffer[ring_head], static_cast<int>(len));
}

// TX pump: queue the next echo chunk, or the final ACK, if the IN endpoint
// is idle. Called after every RX packet and from every TX completion.
/// Refill the IN endpoint from the ring during a big streaming send.
static void big_tx_pump(void) {
    if (!big_transfer_active || big_tx_state != BIG_TX_IDLE)
        return;

#ifdef USB_BRIDGE
    if (big_bridge) {
        if (!usb1_is_configured())
            return;                     // usb1 SET_CONFIGURATION re-pumps
        if (!bridge_hdr_sent) {
            // Forward a header to PC1: [0x02, len_be32, pad] (512 B)
            for (unsigned i = 0; i < sizeof(bridge_hdr_buf); ++i)
                bridge_hdr_buf[i] = 0;
            bridge_hdr_buf[0] = CMD_BIG_TRANSFER;
            bridge_hdr_buf[1] = static_cast<char>((big_total_expected >> 24) & 0xFF);
            bridge_hdr_buf[2] = static_cast<char>((big_total_expected >> 16) & 0xFF);
            bridge_hdr_buf[3] = static_cast<char>((big_total_expected >> 8) & 0xFF);
            bridge_hdr_buf[4] = static_cast<char>(big_total_expected & 0xFF);
            big_tx_state = BIG_TX_HDR;
            if (usb1_tx(bridge_hdr_buf, sizeof(bridge_hdr_buf)) != 0)
                big_tx_state = BIG_TX_IDLE;
            return;
        }
    }
#endif

    if (ring_count > 0) {
        // Send one contiguous chunk from the FIFO (<=BIG_XFER_CHUNK, no wrap)
        uint32_t chunk = ring_count;
        if (chunk > BIG_XFER_CHUNK)
            chunk = BIG_XFER_CHUNK;
        if (chunk > RING_BUF_SIZE - ring_tail)
            chunk = RING_BUF_SIZE - ring_tail;

        // The DWC2 DMA engine ignores the low bits of DIEPDMA, so a TX
        // must never start at an unaligned ring offset. Keep every chunk
        // a multiple of 64 (ring size is too, so wrap preserves this);
        // only the final chunk of the transfer may be an odd size, and by
        // then the tail is aligned. If less than 64 bytes are buffered
        // mid-transfer, wait for more RX data before pumping.
        bool final_chunk = (big_rx_bytes >= big_total_expected) &&
                           (chunk == ring_count);
        if (!final_chunk) {
            chunk &= ~63u;
            if (chunk == 0)
                return;   // next RX completion re-pumps
        }

        big_tx_chunk = chunk;
        big_tx_state = BIG_TX_DATA;
#ifdef USB_BRIDGE
        if (big_bridge) {
            if (usb1_tx(&ring_buffer[ring_tail], static_cast<int>(chunk)) != 0) {
                big_tx_state = BIG_TX_IDLE;
                big_tx_chunk = 0;
            }
            return;
        }
#endif
        usb_vendor_write(&ring_buffer[ring_tail], static_cast<int>(chunk));
        return;
    }

    if (big_rx_bytes >= big_total_expected &&
        big_tx_bytes >= big_total_expected) {
#ifdef USB_BRIDGE
        // Bulk termination for PC1: if the total is an exact multiple of
        // the endpoint MPS the stream has no short packet, so the host's
        // rounded-up read would wait forever — close it with a ZLP.
        static int zlp_sent = 0;
        if (big_bridge && !zlp_sent &&
            (big_total_expected & (BRIDGE_EP_MPS - 1)) == 0) {
            zlp_sent = 1;             // reset via big_reset_state path
            big_tx_state = BIG_TX_DATA;
            big_tx_chunk = 0;
            if (usb1_tx(bridge_hdr_buf, 0) == 0)
                return;
            big_tx_state = BIG_TX_IDLE;
        }
        if (!big_bridge)
            zlp_sent = 0;
        else
            zlp_sent = 0;
#endif
        // Everything received and echoed: send ACK [0x02, len_be32, 'O', 'K']
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

/// Clear all big-transfer bookkeeping back to idle.
static void big_reset_state(void) {
#ifdef USB_BRIDGE
    big_bridge = 0;
    bridge_hdr_sent = 0;
#endif
    big_transfer_active = 0;
    station_sink = 0;
    big_tx_state = BIG_TX_IDLE;
    big_tx_chunk = 0;
    big_rx_stalled = 0;
    ring_head = ring_tail = ring_count = 0;
    big_rx_bytes = big_tx_bytes = big_total_expected = 0;
}

// Common completion path for a data chunk that left the ring (via USB0's
// echo or USB1's bridge endpoint): advance the tail, resume a stalled RX,
// keep pumping.
/// Handle completion of one received big-transfer chunk (credit the ring, re-arm).
static void big_data_chunk_done(void) {
    ring_tail = (ring_tail + big_tx_chunk) % RING_BUF_SIZE;
    ring_count -= big_tx_chunk;
    big_tx_bytes += big_tx_chunk;
    big_tx_chunk = 0;
    big_tx_state = BIG_TX_IDLE;

    // TX freed FIFO space: resume a stalled RX
    if (big_rx_stalled && RING_BUF_SIZE - ring_count >= BIG_XFER_CHUNK) {
        big_rx_stalled = 0;
        if (device_connected && endpoints_enabled)
            big_arm_rx();
    }
    big_tx_pump();
}

/// EP-IN completion callback: advance the send state machine. @param ep Endpoint. @param req Completed request.
static void vendor_ep_in_complete(struct usb_ep *ep, struct usb_request *req) {
    usb_ep_free_request(ep, req);

    if (big_tx_state == BIG_TX_DATA) {
        big_data_chunk_done();
    } else if (big_tx_state == BIG_TX_ACK) {
        // ACK on the wire: transfer finished, resume command reception
        big_reset_state();
        if (device_connected && endpoints_enabled && !read_pending)
            usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
    } else if (station_tx_complete) {
        station_tx_complete();     // a station response/data chunk finished
    }
}

/// EP-OUT completion callback: dispatch a command or credit a big-transfer chunk. @param ep Endpoint. @param req Completed request.
static void vendor_ep_out_complete(struct usb_ep *ep, struct usb_request *req) {
    int ret;

    if (req->status == 0) {
        invalidate_dcache_range(reinterpret_cast<uint32_t>(req->buf), reinterpret_cast<uint32_t>(req->buf) + req->actual);

        if (big_transfer_active) {
            // Zero-copy: the DMA wrote directly into ring_buffer[ring_head]
            uint32_t n = req->actual;
            if (n > RING_BUF_SIZE - ring_count)
                n = RING_BUF_SIZE - ring_count;    // paranoia; RX only armed with >=chunk free
            ring_head = (ring_head + n) % RING_BUF_SIZE;
            ring_count += n;
            big_rx_bytes += n;

#ifndef USB_DEBUG
            // Release builds: one progress dot per received MiB
            if ((big_rx_bytes >> 20) != ((big_rx_bytes - n) >> 20))
                uart_puts(".");
            if (big_rx_bytes >= big_total_expected)
                uart_puts("\n");
#endif

            read_pending = false;
            if (big_rx_bytes < big_total_expected) {
                if (RING_BUF_SIZE - ring_count >= BIG_XFER_CHUNK) {
                    if (device_connected && endpoints_enabled)
                        big_arm_rx();
                } else {
                    big_rx_stalled = 1;   // TX completion will re-arm
                }
            }
            if (!station_sink)
                big_tx_pump();
        } else if (req->actual > 1 && static_cast<char*>(req->buf)[0] == CMD_ECHO) {
            for (int i = 0; i < static_cast<int>(req->actual) - 1 && i < static_cast<int>(USB_DATA_BUF_SIZE); i++) {
                usb_data_buffer[i] = static_cast<char*>(req->buf)[i + 1];
            }
            usb_vendor_write(usb_data_buffer, static_cast<int>(req->actual) - 1);

            read_pending = false;
            if (device_connected && endpoints_enabled) {
                ret = usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
                (void)ret;
            }
        } else if (req->actual >= 5 &&
                   (static_cast<char*>(req->buf)[0] == CMD_BIG_TRANSFER
#ifdef USB_BRIDGE
                    || static_cast<char*>(req->buf)[0] == CMD_BRIDGE
#endif
                   )) {
            // Header: [cmd, len_be32, pad...] — one 512-byte packet, no
            // payload (payload in the header would leave ring_head at an
            // unaligned offset and break the zero-copy DMA). The data
            // stream starts with the next packet, DMA'd into the ring.
            const uint8_t *b = static_cast<const uint8_t*>(req->buf);
            uint32_t total_size = (static_cast<uint32_t>(b[1]) << 24) |
                                  (static_cast<uint32_t>(b[2]) << 16) |
                                  (static_cast<uint32_t>(b[3]) << 8)  |
                                   static_cast<uint32_t>(b[4]);

            big_reset_state();
            big_total_expected = total_size;
            big_transfer_active = 1;
#ifdef USB_BRIDGE
            if (static_cast<char*>(req->buf)[0] == CMD_BRIDGE) {
                big_bridge = 1;
                uart_puts("BRIDGE: forwarding ");
                uart_puts(hex32(total_size));
                uart_puts(" bytes PC0->PC1\n");
            }
#endif

            read_pending = false;
            if (big_rx_bytes < big_total_expected) {
                if (device_connected && endpoints_enabled)
                    big_arm_rx();
            }
            big_tx_pump();
        } else if (req->actual > 0 && station_command &&
                   station_command(static_cast<const uint8_t*>(req->buf),
                                   req->actual)) {
            // Station consumed the command. For PUT it has switched the
            // gadget into sink mode: the ring RX is already armed and
            // read_pending is TRUE — touching it here would let the poll
            // loop arm a competing command read that swallows payload
            // (found on the board: random payload bytes executed as
            // commands). Only the header-only ops re-arm here.
            if (!station_sink) {
                read_pending = false;
                if (device_connected && endpoints_enabled) {
                    ret = usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
                    (void)ret;
                }
            }
        } else {
            read_pending = false;
            if (device_connected && endpoints_enabled) {
                ret = usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
                (void)ret;
            }
        }
    } else {
        read_pending = false;
        if (device_connected && endpoints_enabled) {
            ret = usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
            (void)ret;
        }
    }
    usb_ep_free_request(ep, req);
}

/* ============================ Phase-3 station API =========================
 * Called ONLY from the USB task context (station_command runs inside the
 * RX completion, station_poll inside the poll loop) — the same context as
 * every other gadget callback, so the ring needs no extra locking. */

/// Begin a station/pipeline sink session. @param total Total byte count to receive into the ring.
extern "C" void station_start_sink(uint32_t total) {
    big_reset_state();
    big_total_expected = total;
    big_transfer_active = 1;
    station_sink = 1;
    read_pending = false;
    if (total > 0) {
        if (device_connected && endpoints_enabled)
            big_arm_rx();
    }
}

extern "C" uint32_t station_ring_avail(void) { return ring_count; }

/* Copy up to max bytes out of the ring; advances the tail and resumes a
 * stalled RX, exactly like the echo path's big_data_chunk_done. */
/// Drain received bytes from the sink ring. @param dst Destination. @param max Max bytes. @return bytes copied.
extern "C" uint32_t station_ring_read(void* dst, uint32_t max) {
    uint32_t n = ring_count;
    if (n > max) n = max;
    if (n == 0) return 0;

    uint32_t first = RING_BUF_SIZE - ring_tail;
    if (first > n) first = n;
    memcpy(dst, &ring_buffer[ring_tail], first);
    if (n > first)
        memcpy(static_cast<char*>(dst) + first, &ring_buffer[0], n - first);

    ring_tail = (ring_tail + n) % RING_BUF_SIZE;
    ring_count -= n;

    if (big_rx_stalled && RING_BUF_SIZE - ring_count >= BIG_XFER_CHUNK) {
        big_rx_stalled = 0;
        if (device_connected && endpoints_enabled)
            big_arm_rx();
    }
    return n;
}

/* All expected bytes received AND drained by the consumer. */
extern "C" int station_sink_finished(void) {
    return station_sink && big_rx_bytes >= big_total_expected && ring_count == 0;
}

/* Leave sink mode and resume command reception. */
extern "C" void station_end_sink(void) {
    big_reset_state();
    if (device_connected && endpoints_enabled && !read_pending)
        usb_vendor_read(usb_data_buffer, CMD_READ_SIZE);
}
