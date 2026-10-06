/**
 * @file udc_core.cpp
 * @brief USB Device Controller (UDC) core framework.
 *
 * This file provides the core framework for managing USB Device Controllers,
 * including driver registration, binding, and request mapping. It acts as
 * the glue between the DWC3 controller driver and the gadget framework.
 */

#include <stdint.h>
#include "uboot_compat.hpp"

// Forward declarations (circular — these are also defined in this file)
extern int usb_gadget_register_driver(struct usb_gadget_driver *driver);
extern int usb_gadget_unregister_driver(struct usb_gadget_driver *driver);
extern int usb_gadget_probe_driver(struct usb_gadget_driver *driver);

// ---------------------------------------------------------------------------
// Local helpers (used only in this translation unit)
// ---------------------------------------------------------------------------

// DMA mapping: identity in bare-metal (virtual == physical)
static constexpr int DMA_TO_DEVICE   = 1;
static constexpr int DMA_FROM_DEVICE = 2;

static inline uint32_t dma_map_buf(void *buf, unsigned /*len*/, int /*dir*/)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf));
}
static inline void dma_unmap_buf(void * /*buf*/, unsigned /*len*/, int /*dir*/) {}

static inline int  gadget_connect(struct usb_gadget *g) { return g->ops->pullup(g, 1); }
static inline void gadget_disconnect(struct usb_gadget *g) { g->ops->pullup(g, 0); }

template<typename T>
static inline T udc_min(T a, T b) { return a < b ? a : b; }

inline void set_device_name(struct device* dev, const char* name) {
    (void)dev;
    (void)name;
}

inline constexpr int EISNAM_CODE = 120;

// ---------------------------------------------------------------------------
// Private data structures (local to this file)
// ---------------------------------------------------------------------------

/**
 * @brief Internal structure representing a registered UDC.
 */
struct usb_udc {
    struct usb_gadget_driver *driver; /**< Bound gadget driver */
    struct usb_gadget        *gadget; /**< Underlying gadget hardware */
    struct device             dev;    /**< Device structure for this UDC */
    struct list_head          list;   /**< Node in global UDC list */
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static void          *udc_class = nullptr;
static LIST_HEAD(udc_list);

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

static inline int usb_gadget_udc_start(struct usb_udc *udc)
{
    return udc->gadget->ops->udc_start(udc->gadget, udc->driver);
}

static inline void usb_gadget_udc_stop(struct usb_udc *udc)
{
    udc->gadget->ops->udc_stop(udc->gadget);
}

static void usb_udc_release(struct device *dev)
{
    struct usb_udc *udc = container_of(dev, struct usb_udc, dev);
    free(udc);
}

static void usb_gadget_remove_driver(struct usb_udc *udc)
{
    gadget_disconnect(udc->gadget);
    udc->driver->disconnect(udc->gadget);
    udc->driver->unbind(udc->gadget);
    usb_gadget_udc_stop(udc);
    udc->driver = nullptr;
}

static inline void usb_gadget_udc_set_speed(struct usb_udc *udc,
                                             enum usb_device_speed speed)
{
    if (udc->gadget->ops->udc_set_speed) {
        const enum usb_device_speed s =
            udc_min(speed, udc->gadget->max_speed);
        udc->gadget->ops->udc_set_speed(udc->gadget, s);
    }
}

static int udc_bind_to_driver(struct usb_udc *udc,
                               struct usb_gadget_driver *driver)
{
    uart_puts("UDC: udc_bind_to_driver started\n");

    udc->driver = driver;
    usb_gadget_udc_set_speed(udc, driver->speed);

    uart_puts("UDC: calling driver->bind\n");
    int ret = driver->bind(udc->gadget);
    if (ret) {
        char buf[64];
        snprintf(buf, sizeof(buf), "UDC: driver->bind FAILED: %d\n", ret);
        uart_puts(buf);
        goto err1;
    }
    uart_puts("UDC: driver->bind OK\n");

    uart_puts("UDC: calling usb_gadget_udc_start\n");
    mdelay(10);
    ret = usb_gadget_udc_start(udc);
    if (ret) {
        char buf[64];
        snprintf(buf, sizeof(buf), "UDC: usb_gadget_udc_start FAILED: %d\n", ret);
        uart_puts(buf);
        driver->unbind(udc->gadget);
        goto err1;
    }
    uart_puts("UDC: usb_gadget_udc_start OK\n");

    uart_puts("UDC: calling usb_gadget_connect\n");
    {
        const int pullup_ret = gadget_connect(udc->gadget);
        char buf[64];
        snprintf(buf, sizeof(buf), "UDC: usb_gadget_connect ret=%d\n", pullup_ret);
        uart_puts(buf);
    }
    return 0;

err1:
    if (ret != -EISNAM_CODE)
        printf("failed to start %s: %d\n", udc->driver->function, ret);
    udc->driver = nullptr;
    return ret;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

/**
 * @brief Maps a USB request's buffer for DMA.
 * @param gadget Pointer to the gadget.
 * @param req Pointer to the request.
 * @param is_in True if this is an IN transfer.
 * @return 0 on success.
 */
int usb_gadget_map_request(struct usb_gadget *gadget,
                           struct usb_request *req, int is_in)
{
    if (req->length == 0)
        return 0;
    req->dma = dma_map_buf(req->buf, req->length,
                           is_in ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
    return 0;
}

void usb_gadget_unmap_request(struct usb_gadget *gadget,
                              struct usb_request *req, int is_in)
{
    if (req->length == 0)
        return;
    dma_unmap_buf(reinterpret_cast<void *>(static_cast<uintptr_t>(req->dma)),
                  req->length,
                  is_in ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}

void usb_gadget_giveback_request(struct usb_ep *ep, struct usb_request *req)
{
    req->complete(ep, req);
}

void usb_gadget_set_state(struct usb_gadget *gadget,
                          enum usb_device_state state)
{
    gadget->state = state;
}

void usb_gadget_udc_reset(struct usb_gadget *gadget,
                          struct usb_gadget_driver *driver)
{
    driver->reset(gadget);
    usb_gadget_set_state(gadget, USB_STATE_DEFAULT);
}

/**
 * @brief Adds a new gadget UDC to the core framework.
 * @param parent Pointer to the parent device.
 * @param gadget Pointer to the gadget structure.
 * @param release Release callback (optional).
 * @return 0 on success, negative error code on failure.
 */
int usb_add_gadget_udc_release(struct device *parent,
                               struct usb_gadget *gadget,
                               void (*release)(struct device *dev))
{
    struct usb_udc *udc = static_cast<struct usb_udc *>(
        malloc(sizeof(*udc)));
    if (!udc)
        return -ENOMEM;

    memset(udc, 0, sizeof(*udc));
    set_device_name(&gadget->dev, "gadget");
    gadget->dev.parent   = parent;

    udc->dev.release     = usb_udc_release;
    udc->dev._class_placeholder = static_cast<struct device *>(udc_class);
    udc->dev.parent      = parent;
    udc->gadget          = gadget;

    list_add_tail(&udc->list, &udc_list);
    usb_gadget_set_state(gadget, USB_STATE_NOTATTACHED);
    return 0;
}

int usb_add_gadget_udc(struct device *parent, struct usb_gadget *gadget)
{
    return usb_add_gadget_udc_release(parent, gadget, nullptr);
}

void usb_del_gadget_udc(struct usb_gadget *gadget)
{
    struct usb_udc *udc = nullptr;

    list_for_each_entry(udc, &udc_list, list)
        if (udc->gadget == gadget)
            goto found;

    printf("gadget not registered.\n");
    return;

found:
    list_del(&udc->list);
    if (udc->driver)
        usb_gadget_remove_driver(udc);
}

/**
 * @brief Probes a gadget driver against available UDCs and binds if possible.
 * @param driver Pointer to the gadget driver.
 * @return 0 on success, negative error code on failure.
 */
int usb_gadget_probe_driver(struct usb_gadget_driver *driver)
{
    uart_puts("UDC: probe_driver called\n");
    if (!driver || !driver->bind || !driver->setup) {
        uart_puts("UDC: probe_driver - invalid params\n");
        return -EINVAL;
    }

    uart_puts("UDC: Looking for available UDC...\n");
    struct usb_udc *udc = nullptr;
    int count = 0;

    list_for_each_entry(udc, &udc_list, list) {
        ++count;
        char buf[64];
        snprintf(buf, sizeof(buf), "UDC: Found udc gadget=%p driver=%p\n",
                 static_cast<void *>(udc->gadget),
                 static_cast<void *>(udc->driver));
        uart_puts(buf);
        if (!udc->driver)
            goto found;
    }
    {
        char buf2[64];
        snprintf(buf2, sizeof(buf2),
                 "UDC: couldn't find an available UDC (count=%d)\n", count);
        uart_puts(buf2);
    }
    return -ENODEV;

found:
    return udc_bind_to_driver(udc, driver);
}

int usb_gadget_register_driver(struct usb_gadget_driver *driver)
{
    return usb_gadget_probe_driver(driver);
}

int usb_gadget_unregister_driver(struct usb_gadget_driver *driver)
{
    struct usb_udc *udc = nullptr;
    int ret = -ENODEV;

    if (!driver || !driver->unbind)
        return -EINVAL;

    list_for_each_entry(udc, &udc_list, list)
        if (udc->driver == driver) {
            usb_gadget_remove_driver(udc);
            usb_gadget_set_state(udc->gadget, USB_STATE_NOTATTACHED);
            ret = 0;
            break;
        }

    return ret;
}

