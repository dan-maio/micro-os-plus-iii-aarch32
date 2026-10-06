/**
 * @file uboot_compat.hpp
 * @brief U-Boot compatibility types and macros for bare-metal USB.
 */

#ifndef UBOOT_COMPAT_HPP_
#define UBOOT_COMPAT_HPP_

#include <stdint.h>

// BIT() defined before extern "C" so constexpr constants using it compile correctly
constexpr uint32_t BIT(unsigned n) { return 1U << n; }

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short dev_t;

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

struct device {
    struct device *parent;
    struct device *_class_placeholder;
    dev_t devt;
    void (*release)(struct device *dev);
    void *driver_data;
    void *device_data;
};

struct usb_descriptor_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

struct usb_string {
    uint8_t id;
    const char *s;
};

struct usb_gadget_strings {
    uint16_t language;
    struct usb_string *strings;
};

struct usb_ctrlrequest {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_ep;
struct usb_request;
struct scatterlist;

struct usb_request {
    void *buf;
    unsigned length;
    uint32_t dma;
    unsigned stream_id:16;
    unsigned no_interrupt:1;
    unsigned zero:1;
    unsigned short_not_ok:1;
    unsigned is_last:1;
    void (*complete)(struct usb_ep *ep, struct usb_request *req);
    void *context;
    struct list_head list;
    int status;
    unsigned actual;
    struct scatterlist *sg;
    unsigned num_sgs;
    unsigned num_mapped_sgs;
    unsigned int frame_number;
};

struct usb_ep_ops {
    int (*enable)(struct usb_ep *ep, const struct usb_endpoint_descriptor *desc);
    int (*disable)(struct usb_ep *ep);
    struct usb_request *(*alloc_request)(struct usb_ep *ep, unsigned gfp_flags);
    void (*free_request)(struct usb_ep *ep, struct usb_request *req);
    int (*queue)(struct usb_ep *ep, struct usb_request *req, unsigned gfp_flags);
    int (*dequeue)(struct usb_ep *ep, struct usb_request *req);
    int (*set_halt)(struct usb_ep *ep, int value);
    int (*set_wedge)(struct usb_ep *ep);
    int (*fifo_status)(struct usb_ep *ep);
    void (*fifo_flush)(struct usb_ep *ep);
};

struct usb_ss_ep_comp_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

struct usb_ep_caps {
    unsigned type_control:1;
    unsigned type_iso:1;
    unsigned type_bulk:1;
    unsigned type_int:1;
    unsigned dir_in:1;
    unsigned dir_out:1;
};

struct usb_ep {
    void *driver_data;
    const char *name;
    const struct usb_ep_ops *ops;
    struct list_head ep_list;
    struct usb_ep_caps caps;
    unsigned maxpacket:16;
    unsigned maxpacket_limit:16;
    unsigned max_streams:16;
    unsigned maxburst:5;
    const struct usb_endpoint_descriptor *desc;
    const struct usb_ss_ep_comp_descriptor *comp_desc;
};

struct usb_gadget_ops {
    int (*get_frame)(struct usb_gadget *g);
    int (*wakeup)(struct usb_gadget *g);
    int (*set_selfpowered)(struct usb_gadget *g, int is_selfpowered);
    int (*vbus_session)(struct usb_gadget *g, int is_active);
    int (*vbus_draw)(struct usb_gadget *g, unsigned mA);
    int (*pullup)(struct usb_gadget *g, int is_on);
    int (*ioctl)(struct usb_gadget *g, unsigned code, unsigned long param);
    int (*udc_start)(struct usb_gadget *g, struct usb_gadget_driver *driver);
    int (*udc_stop)(struct usb_gadget *g);
    struct usb_ep *(*match_ep)(struct usb_gadget *g,
            struct usb_endpoint_descriptor *,
            struct usb_ss_ep_comp_descriptor *);
    void (*udc_set_speed)(struct usb_gadget *g, unsigned int speed);
};

enum usb_device_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW = 1,
    USB_SPEED_FULL = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_WIRELESS = 4,
    USB_SPEED_SUPER = 5,
    USB_SPEED_SUPER_PLUS = 6,
};

struct usb_gadget_driver {
    char *function;
    enum usb_device_speed speed;
    const struct usb_device_descriptor *dev;
    struct usb_gadget_strings **strings;
    int (*bind)(struct usb_gadget *);
    void (*unbind)(struct usb_gadget *);
    int (*setup)(struct usb_gadget *, const struct usb_ctrlrequest *);
    void (*disconnect)(struct usb_gadget *);
    void (*suspend)(struct usb_gadget *);
    void (*resume)(struct usb_gadget *);
    void (*reset)(struct usb_gadget *);
};

enum usb_device_state {
    USB_STATE_NOTATTACHED = 0,
    USB_STATE_ATTACHED,
    USB_STATE_POWERED,
    USB_STATE_RECONNECTING,
    USB_STATE_UNAUTHENTICATED,
    USB_STATE_DEFAULT,
    USB_STATE_ADDRESS,
    USB_STATE_CONFIGURED,
    USB_STATE_SUSPENDED,
};

struct usb_gadget {
    const struct usb_gadget_ops *ops;
    struct usb_ep *ep0;
    struct list_head ep_list;
    enum usb_device_speed speed;
    enum usb_device_speed max_speed;
    enum usb_device_state state;
    unsigned is_dualspeed:1;
    unsigned is_otg:1;
    unsigned is_a_peripheral:1;
    unsigned b_hnp_enable:1;
    unsigned a_hnp_support:1;
    unsigned a_alt_hnp_support:1;
    unsigned is_selfpowered:1;
    const char *name;
    struct device dev;
    unsigned quirk_ep_out_aligned_size:1;
    uint16_t isoch_delay;
    void *hcd_priv;
};

struct usb_function;

struct usb_configuration {
    const char *label;
    struct usb_gadget_strings **strings;
    const struct usb_descriptor_header **descriptors;
    int (*bind)(struct usb_configuration *);
    void (*unbind)(struct usb_configuration *);
    int (*setup)(struct usb_configuration *, const struct usb_ctrlrequest *);
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
    struct usb_composite_dev *cdev;
    struct list_head list;
    struct list_head functions;
    uint8_t next_interface_id;
    unsigned superspeed:1;
    unsigned highspeed:1;
    unsigned fullspeed:1;
    struct usb_function *interface[16];
};

struct usb_composite_dev {
    struct usb_gadget *gadget;
    struct usb_request *req;
    unsigned bufsiz;
    struct usb_configuration *config;
    unsigned int suspended:1;
    struct usb_device_descriptor desc __attribute__((aligned(64)));
    struct list_head configs;
    struct usb_composite_driver *driver;
    uint8_t next_string_id;
    unsigned deactivations;
};

struct usb_function {
    const char *name;
    struct usb_gadget_strings **strings;
    struct usb_descriptor_header **descriptors;
    struct usb_descriptor_header **hs_descriptors;
    struct usb_descriptor_header **ss_descriptors;
    struct usb_configuration *config;
    int (*bind)(struct usb_configuration *, struct usb_function *);
    void (*unbind)(struct usb_configuration *, struct usb_function *);
    int (*set_alt)(struct usb_function *, unsigned interface, unsigned alt);
    int (*get_alt)(struct usb_function *, unsigned interface);
    void (*disable)(struct usb_function *);
    int (*setup)(struct usb_function *, const struct usb_ctrlrequest *);
    void (*suspend)(struct usb_function *);
    void (*resume)(struct usb_function *);
    struct list_head list;
    unsigned long endpoints[1];
};

struct usb_composite_driver {
    const char *name;
    const struct usb_device_descriptor *dev;
    struct usb_gadget_strings **strings;
    int (*bind)(struct usb_composite_dev *);
    int (*unbind)(struct usb_composite_dev *);
    void (*disconnect)(struct usb_composite_dev *);
    void (*suspend)(struct usb_composite_dev *);
    void (*resume)(struct usb_composite_dev *);
};

struct scatterlist;

// USB configuration descriptor
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

// Function declarations needed by multiple C++ files
extern void  *memcpy(void *dest, const void *src, unsigned long n);
extern unsigned long strlen(const char *s);
extern int    strcmp(const char *s1, const char *s2);
extern char  *strrchr(const char *s, int c);
extern int    snprintf(char *buf, unsigned long size, const char *fmt, ...);
extern int    printf(const char *fmt, ...);
extern void  *malloc(unsigned long size);
extern void   free(void *ptr);
extern unsigned long simple_strtoul(const char *s, char **ep, unsigned int base);


static inline int usb_ep_enable(struct usb_ep *ep, const struct usb_endpoint_descriptor *desc) {
    return ep->ops->enable(ep, desc);
}

static inline int usb_ep_disable(struct usb_ep *ep) {
    return ep->ops->disable(ep);
}

static inline struct usb_request *usb_ep_alloc_request(struct usb_ep *ep, unsigned gfp_flags) {
    return ep->ops->alloc_request(ep, gfp_flags);
}

static inline void usb_ep_free_request(struct usb_ep *ep, struct usb_request *req) {
    ep->ops->free_request(ep, req);
}

static inline int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, unsigned gfp_flags) {
    return ep->ops->queue(ep, req, gfp_flags);
}

void uart_puts(const char *s);
char *hex32(uint32_t val);
void mdelay(unsigned long msec);
void invalidate_dcache_range(uint32_t start, uint32_t stop);
void flush_dcache_range(uint32_t start, uint32_t stop);
void *memset(void *s, int c, unsigned long n);
void *memalign(unsigned long alignment, unsigned long size);
void *kzalloc(unsigned long size, unsigned int flags);
extern void *calloc(unsigned long count, unsigned long size);
extern int   strncmp(const char *s1, const char *s2, unsigned long n);

// USB descriptor structures (used by composite layer)
struct usb_qualifier_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint8_t  bNumConfigurations;
    uint8_t  bRESERVED;
} __attribute__((packed));

struct usb_string_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wData[127];
} __attribute__((packed));

struct usb_bos_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumDeviceCaps;
} __attribute__((packed));

struct usb_dev_cap_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDevCapabilityType;
} __attribute__((packed));

struct usb_ext_cap_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDevCapabilityType;
    uint32_t bmAttributes;
} __attribute__((packed));

struct usb_ss_cap_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDevCapabilityType;
    uint8_t  bmAttributes;
    uint16_t wSpeedSupported;
    uint8_t  bFunctionalitySupport;
    uint8_t  bU1devExitLat;
    uint16_t bU2DevExitLat;
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

// C++ linkage declarations — functions defined in C++ translation units
[[nodiscard]] int usb_interface_id(struct usb_configuration *c, struct usb_function *f);
[[nodiscard]] struct usb_ep *usb_ep_autoconfig(struct usb_gadget *gadget, struct usb_endpoint_descriptor *ep);
void usb_ep_autoconfig_reset(struct usb_gadget *gadget);
[[nodiscard]] int usb_add_function(struct usb_configuration *c, struct usb_function *f);
[[nodiscard]] int usb_add_config(struct usb_composite_dev *cdev, struct usb_configuration *config);
[[nodiscard]] int usb_composite_register(struct usb_composite_driver *driver);

void usb_gadget_set_state(struct usb_gadget *gadget, enum usb_device_state state);
[[nodiscard]] int usb_gadget_map_request(struct usb_gadget *gadget, struct usb_request *req, int direction);
void usb_gadget_unmap_request(struct usb_gadget *gadget, struct usb_request *req, int direction);
void usb_gadget_giveback_request(struct usb_ep *ep, struct usb_request *req);
void usb_gadget_udc_reset(struct usb_gadget *gadget, struct usb_gadget_driver *driver);
[[nodiscard]] int usb_add_gadget_udc(struct device *parent, struct usb_gadget *gadget);
void usb_del_gadget_udc(struct usb_gadget *gadget);

[[nodiscard]] extern int  usb_gadget_get_string(struct usb_gadget_strings *table, int id, uint8_t *buf);
[[nodiscard]] extern int  usb_descriptor_fillbuf(void *buf, unsigned buflen,
                                   const struct usb_descriptor_header **src);
[[nodiscard]] extern int  usb_gadget_register_driver(struct usb_gadget_driver *driver);
[[nodiscard]] extern int  usb_gadget_unregister_driver(struct usb_gadget_driver *driver);
void        usb_composite_unregister(struct usb_composite_driver *driver);

// ---------------------------------------------------------------------------
// USB constants (formerly #defines)
// ---------------------------------------------------------------------------

// USB descriptor type codes
constexpr uint8_t USB_DT_DEVICE                  = 1;
constexpr uint8_t UBOOT_USB_DT_CONFIG             = 2;
constexpr uint8_t USB_DT_CONFIG                   = 2;
constexpr uint8_t UBOOT_USB_DT_STRING             = 3;
constexpr uint8_t USB_DT_STRING                   = 3;
constexpr uint8_t USB_DT_DEVICE_QUALIFIER         = 6;
constexpr uint8_t USB_DT_OTHER_SPEED_CONFIG       = 7;
constexpr uint8_t USB_DT_BOS                      = 15;
constexpr uint8_t USB_DT_INTERFACE                = 4;
constexpr uint8_t USB_DT_ENDPOINT                 = 5;

// Descriptor sizes
constexpr uint8_t USB_DT_INTERFACE_SIZE           = 9;
constexpr uint8_t USB_DT_ENDPOINT_SIZE            = 7;
constexpr uint8_t USB_DT_CONFIG_SIZE              = 9;
constexpr uint8_t USB_DT_DEVICE_SIZE              = 18;

// Endpoint direction
constexpr uint8_t USB_DIR_OUT                     = 0;
constexpr uint8_t USB_DIR_IN                      = 0x80;

// Endpoint masks and transfer types
constexpr uint8_t USB_ENDPOINT_DIR_MASK           = 0x80;
constexpr uint8_t USB_ENDPOINT_NUMBER_MASK        = 0x0f;
constexpr uint8_t USB_ENDPOINT_XFER_CONTROL       = 0;
constexpr uint8_t USB_ENDPOINT_XFER_ISOC          = 1;
constexpr uint8_t USB_ENDPOINT_XFER_BULK          = 2;
constexpr uint8_t USB_ENDPOINT_XFER_INT           = 3;
constexpr uint8_t USB_ENDPOINT_XFERTYPE_MASK      = 0x03;
constexpr uint8_t USB_ENDPOINT_HALT               = 0;

// Recipient bits
constexpr uint8_t USB_RECIP_MASK                  = 0x1f;
constexpr uint8_t USB_RECIP_DEVICE                = 0;
constexpr uint8_t USB_RECIP_INTERFACE             = 1;
constexpr uint8_t USB_RECIP_ENDPOINT              = 2;
constexpr uint8_t USB_RECIP_OTHER                 = 3;

// Device feature selectors
constexpr int USB_DEVICE_REMOTE_WAKEUP            = 1;
constexpr int USB_DEVICE_U1_ENABLE                = 2;
constexpr int USB_DEVICE_U2_ENABLE                = 3;
constexpr int USB_DEVICE_LTM_ENABLE               = 4;
constexpr int USB_DEVICE_TEST_MODE                = 5;

// Interface function suspend
constexpr int USB_INTRF_FUNC_SUSPEND              = 0;
constexpr int USB_GADGET_DELAYED_STATUS           = 0x7fff;

// Standard request codes
constexpr uint8_t USB_REQ_GET_STATUS              = 0x00;
constexpr uint8_t USB_REQ_GET_DESCRIPTOR          = 0x06;
constexpr uint8_t USB_REQ_CLEAR_FEATURE           = 0x01;
constexpr uint8_t USB_REQ_SET_FEATURE             = 0x03;
constexpr uint8_t USB_REQ_SET_ADDRESS             = 0x05;
constexpr uint8_t USB_REQ_SET_CONFIGURATION       = 0x09;
constexpr uint8_t USB_REQ_SET_SEL                 = 0x30;
constexpr uint8_t USB_REQ_SET_ISOCH_DELAY         = 0x31;
constexpr uint8_t USB_REQ_GET_CONFIGURATION       = 0x08;
constexpr uint8_t USB_REQ_SET_INTERFACE           = 0x0B;
constexpr uint8_t USB_REQ_GET_INTERFACE           = 0x0A;

// Request type bits
constexpr uint8_t USB_TYPE_MASK                   = 0x60;
constexpr uint8_t USB_TYPE_STANDARD               = (0x00u << 5);

// Device status bits
constexpr uint32_t USB_DEV_STAT_U1_ENABLED        = BIT(2);
constexpr uint32_t USB_DEV_STAT_U2_ENABLED        = BIT(3);

// LE/CPU byte-order conversion (identity on little-endian)
constexpr uint16_t le16_to_cpu(uint16_t x) { return x; }
constexpr uint16_t cpu_to_le16(uint16_t x) { return x; }

// Configuration attributes
constexpr uint8_t USB_CONFIG_ATT_ONE              = 0x80;
constexpr uint8_t USB_CONFIG_ATT_SELFPOWER        = 0x40;

// GFP flags (bare-metal: all zero)
constexpr int GFP_ATOMIC                          = 0;
constexpr int GFP_KERNEL                          = 0;

// Error codes
constexpr int ENODEV                              = 19;
constexpr int ENOMEM                              = 12;
constexpr int EINVAL                              = 22;
constexpr int EBUSY                               = 16;
constexpr int ETIMEDOUT                           = 110;
constexpr int EINPROGRESS                         = 115;
constexpr int ESHUTDOWN                           = 117;
constexpr int ECONNRESET                          = 104;
constexpr int EPIPE                               = 32;
constexpr int EISNAM                              = 120;
constexpr int ENOENT                              = 2;
constexpr int EOPNOTSUPP                          = 95;

// __le16 type alias
using __le16 = uint16_t;

// __constant_cpu_to_le16: compile-time identity cast
constexpr uint16_t __constant_cpu_to_le16(uint16_t x) { return x; }

// USB BOS / capability descriptor
constexpr uint8_t  USB_DT_DEVICE_CAPABILITY       = 0x10;
constexpr uint8_t  USB_DT_BOS_SIZE                = 5;
constexpr uint8_t  USB_DT_USB_EXT_CAP_SIZE        = 7;
constexpr uint8_t  USB_DT_USB_SS_CAP_SIZE         = 10;
constexpr uint8_t  USB_CAP_TYPE_EXT               = 2;
constexpr uint32_t USB_LPM_SUPPORT                = (1u << 1);
constexpr uint8_t  USB_SS_CAP_TYPE                = 3;
constexpr uint32_t USB_FULL_SPEED_OPERATION       = (1u << 0);
constexpr uint32_t USB_HIGH_SPEED_OPERATION       = (1u << 1);
constexpr uint32_t USB_5GBPS_OPERATION            = (1u << 3);
constexpr uint8_t  USB_DEFAULT_U1_DEV_EXIT_LAT    = 0x01u;
constexpr uint16_t USB_DEFAULT_U2_DEV_EXIT_LAT    = 0x07FFu;

// Composite-layer limits
constexpr int MAX_CONFIG_INTERFACES               = 16;
constexpr int CONFIG_USB_GADGET_VBUS_DRAW         = 500;
constexpr int USB_BUFSIZ                          = 4096;
constexpr int CONFIG_SYS_CACHELINE_SIZE           = 64;

// ---------------------------------------------------------------------------
// Macros that require the preprocessor (type injection, typeof, variadic)
// ---------------------------------------------------------------------------

#define min_t(type, x, y)  ((type)(x) < (type)(y) ? (type)(x) : (type)(y))
#define max_t(type, x, y)  ((type)(x) > (type)(y) ? (type)(x) : (type)(y))

// debug — no-op
#ifndef debug
#define debug(fmt, ...) do {} while(0)
#endif

// ---------------------------------------------------------------------------
// List helpers — skipped when dwc3_compat.hpp is in scope
// ---------------------------------------------------------------------------
#ifndef DWC3_COMPAT_HPP_
#define LIST_HEAD_INIT(name)  { &(name), &(name) }
#define LIST_HEAD(name)       struct list_head name = LIST_HEAD_INIT(name)
#define INIT_LIST_HEAD(ptr)   do { (ptr)->next = (ptr)->prev = (ptr); } while (0)

static inline int list_empty(const struct list_head *h) { return h->next == h; }

#define list_first_entry(ptr, type, member) \
    container_of((ptr)->next, type, member)

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#define list_for_each_entry(pos, head, member)                              \
    for ((pos) = container_of((head)->next, __typeof__(*(pos)), member);    \
         &(pos)->member != (head);                                           \
         (pos) = container_of((pos)->member.next, __typeof__(*(pos)), member))

static inline void list_add_tail(struct list_head *n, struct list_head *head) {
    struct list_head *prev = head->prev;
    n->next = head; n->prev = prev;
    prev->next = n; head->prev = n;
}
static inline void list_del(struct list_head *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}
#endif // !DWC3_COMPAT_HPP_

// Unaligned 16-bit LE access
static inline uint16_t get_unaligned_le16(const void *p) {
    uint16_t v; __builtin_memcpy(&v, p, 2); return v;
}
static inline void put_unaligned_le16(uint16_t val, void *p) {
    __builtin_memcpy(p, &val, 2);
}
#define get_unaligned(ptr) \
    ({ __typeof__(*(ptr)) _v; __builtin_memcpy(&_v, (ptr), sizeof(_v)); _v; })
#define put_unaligned(val, ptr) \
    do { __typeof__(*(ptr)) _v = (val); __builtin_memcpy((ptr), &_v, sizeof(_v)); } while (0)

// gadget_is_* chip detection — always false except dwc3
static inline bool gadget_is_net2280(struct usb_gadget *) { return false; }
static inline bool gadget_is_goku(struct usb_gadget *)    { return false; }
static inline bool gadget_is_sh(struct usb_gadget *)      { return false; }
static inline bool gadget_is_mq11xx(struct usb_gadget *)  { return false; }
static inline bool gadget_is_pxa(struct usb_gadget *)     { return false; }
static inline bool gadget_is_dwc3(struct usb_gadget *g)   { return !strcmp("dwc3-gadget", g->name); }

// min template
template<typename T> constexpr T min(T a, T b) { return a < b ? a : b; }

// max template
template<typename T> constexpr T max(T a, T b) { return a > b ? a : b; }

// kfree = free in bare-metal
static inline void kfree(void *p) { free(p); }

// BUG_ON
static inline void BUG_ON(bool cond) { if (cond) uart_puts("BUG!\n"); }

// Bit operations for USB endpoint bitmaps (endpoints[1] = single unsigned long)
static inline void bitmap_zero(unsigned long *bm, int) { *bm = 0UL; }
static inline void generic_set_bit(int nr, unsigned long *bm) { *bm |= (1UL << (unsigned)nr); }
static inline bool test_bit(int nr, const unsigned long *bm) { return (*bm >> (unsigned)nr) & 1UL; }

// Gadget chip detection — skipped for dwc3_compat.hpp
#ifndef DWC3_COMPAT_HPP_
static inline bool gadget_is_dualspeed(struct usb_gadget *g)  { return g->is_dualspeed; }
static inline bool gadget_is_superspeed(struct usb_gadget *)   { return false; }
static inline bool gadget_is_otg(struct usb_gadget *g)         { return g->is_otg; }
#endif // !DWC3_COMPAT_HPP_

// Gadget control inlines
static inline void usb_gadget_vbus_draw(struct usb_gadget *g, unsigned mA) {
    if (g->ops->vbus_draw) g->ops->vbus_draw(g, mA);
}
static inline int usb_gadget_connect(struct usb_gadget *g) {
    return g->ops->pullup(g, 1);
}
static inline int usb_gadget_disconnect(struct usb_gadget *g) {
    return g->ops->pullup(g, 0);
}
static inline void usb_gadget_set_selfpowered(struct usb_gadget *g) {
    g->is_selfpowered = 1;
    if (g->ops->set_selfpowered) g->ops->set_selfpowered(g, 1);
}

// Gadget driver_data accessors
static inline void *get_gadget_data(struct usb_gadget *g) { return g->dev.driver_data; }
template<typename T>
static inline void set_gadget_data(struct usb_gadget *g, T *d) {
    g->dev.driver_data = reinterpret_cast<void*>(d);
}
static inline void set_gadget_data(struct usb_gadget *g, decltype(nullptr)) {
    g->dev.driver_data = nullptr;
}

#endif
