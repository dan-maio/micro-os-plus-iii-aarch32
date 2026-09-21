/**
 * @file dwc2_driver.hpp
 * @brief High-level C++23 driver interface for the DWC2 USB Controller on RK3506.
 */

#ifndef DWC2_DRIVER_HPP_
#define DWC2_DRIVER_HPP_

#include <stdint.h>
#include "dwc2_regs.hpp"
#include "usb_device_state.hpp"

// U-Boot gadget framework structures (defined in uboot_compat.hpp)
struct usb_ep;
struct usb_request;

namespace rk3506::usb::dwc2 {

/** @brief Standard status/result types matching the composite framework. */
enum class DriverStatus : int {
    OK = 0,
    ERROR = -1,
    TIMEOUT = -2,
    INVALID_ARG = -3
};

/**
 * @class Driver
 * @brief C++23 Singleton Driver representing the DWC2 controller.
 */
class Driver {
public:
    /** @brief Get the singleton instance of the driver. */
    static Driver& instance() {
        static Driver drv;
        return drv;
    }

    /**
     * @brief Initialize registers, clocks, resets, and PHY configuration for RK3506.
     * @return DriverStatus
     */
    DriverStatus initialize() noexcept;

    /**
     * @brief Performs a core soft reset of the DWC2 IP.
     * @return DriverStatus
     */
    DriverStatus reset() noexcept;

    /**
     * @brief Allocates internal RAM to the RX FIFO and logical TX FIFOs.
     */
    void configure_fifos() noexcept;

    /**
     * @brief Enables global DWC2 interrupts (Reset, EnumDone, Suspend, IN/OUT EPs).
     */
    void enable_interrupts() noexcept;

    /**
     * @brief Registers the hardware GIC interrupt routing.
     */
    void configure_gic() noexcept;

    /**
     * @brief Set the USB Device Address.
     * @param address Address allocated by the host (0 to 127).
     */
    void set_address(uint8_t address) noexcept;

    /**
     * @brief Configure and start a DMA transmit (IN transfer).
     * @param ep_num Endpoint logical index.
     * @param buf Pointer to the TX buffer.
     * @param len Total length of data.
     * @param fifo_num FIFO index associated with this endpoint.
     */
    void start_tx(uint8_t ep_num, const void* buf, uint32_t len, uint8_t fifo_num) noexcept;

    /**
     * @brief Configure and start a DMA receive (OUT transfer).
     * @param ep_num Endpoint logical index.
     * @param buf Pointer to the RX buffer.
     * @param len Maximum expected length.
     */
    void start_rx(uint8_t ep_num, void* buf, uint32_t len) noexcept;

    /**
     * @brief Stall or unstall an endpoint.
     * @param ep_num Endpoint logical index.
     * @param is_in Direction (true = IN, false = OUT).
     * @param stall_ep True to assert STALL, false to clear STALL.
     */
    void stall(uint8_t ep_num, bool is_in, bool stall_ep) noexcept;

    /**
     * @brief Check if the controller is in high-speed or full-speed mode.
     * @return True if High Speed (negotiated).
     */
    bool is_high_speed() const noexcept;

    /**
     * @brief Main interrupt service handler (polled or GIC-triggered).
     */
    void handle_interrupt() noexcept;

    /**
     * @brief Record the active request on an endpoint so the interrupt
     *        handler can invoke its completion callback on TRANSFER_DONE.
     *        Buffer DMA mode allows one active transfer per endpoint.
     * @param ep_num Endpoint logical index (1..15; EP0 is handled internally).
     * @param is_in  Direction (true = IN, false = OUT).
     * @param ep     Gadget framework endpoint owning the request.
     * @param req    Request being started (nullptr to clear the slot).
     */
    void set_pending_request(uint8_t ep_num, bool is_in,
                             struct usb_ep* ep, struct usb_request* req) noexcept;

    /**
     * @brief Direction of the next EP0 transfer queued for the SETUP packet
     *        currently being serviced. Follows bmRequestType bit 7 when a
     *        data stage exists; a no-data request (wLength == 0) has only an
     *        IN status stage, so the queued ZLP must go out EP0-IN.
     * @return True if device-to-host (IN).
     */
    bool ep0_data_in() const noexcept { return ep0_next_in_; }

    /** @brief Sets current device state. */
    void set_state(rk3506::usb::State state) noexcept { state_ = state; }

    /** @brief Gets current device state. */
    rk3506::usb::State get_state() const noexcept { return state_; }

private:
    Driver() noexcept;
    
    // Internal helper routines
    void process_ep_in_interrupt() noexcept;
    void process_ep_out_interrupt() noexcept;
    void handle_ep0_setup() noexcept;
    void complete_pending(uint8_t ep_num, bool is_in) noexcept;

    // One active request per endpoint and direction (Buffer DMA mode)
    struct PendingXfer {
        struct usb_ep*      ep;
        struct usb_request* req;
    };
    PendingXfer pending_in_[16];
    PendingXfer pending_out_[16];

    ControllerRegs* regs_;
    rk3506::usb::State state_;
    uint8_t address_;
    bool connected_;
    bool initialized_ = false;
    // Direction of the next EP0 transfer for the in-flight control request
    // (data stage direction, or IN for the status ZLP of a no-data request)
    bool ep0_next_in_ = false;
    
    // Aligned setup buffer (DMA target: 64-byte aligned, and static so it
    // can be placed in the Normal-NC .dma_nc window — the singleton object
    // itself lives in cacheable .bss). Defined in dwc2_driver.cpp.
    static uint8_t setup_buffer_[64];
};

} // namespace rk3506::usb::dwc2

#endif // DWC2_DRIVER_HPP_
