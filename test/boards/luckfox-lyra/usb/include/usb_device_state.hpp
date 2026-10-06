/**
 * @file usb_device_state.hpp
 * @brief USB device state tracking and connection status.
 */

#ifndef USB_DEVICE_STATE_HPP_
#define USB_DEVICE_STATE_HPP_

#include <stdint.h>

extern bool device_connected;
extern bool endpoints_enabled;
extern bool read_pending;

#ifdef __cplusplus
namespace rk3506 {
namespace usb {

enum class State {
    disconnected = 0,
    powered,
    default_state,
    addressed,
    configured,
    suspended
};

class DeviceState {

public:
    static DeviceState& instance() {
        static DeviceState state;
        return state;
    }

    void initialize() {
        device_connected = false;
        endpoints_enabled = false;
        read_pending = false;
        
        poll_count_ = 0;
        connected_count_ = 0;
    }

    bool is_connected() const { return device_connected; }
    bool are_enabled() const { return endpoints_enabled; }
    bool is_pending() const { return read_pending; }

    void set_connected(bool c) {
        device_connected = c;
        if (c) connected_count_++;
    }

    void set_enabled(bool e) { endpoints_enabled = e; }
    void set_pending(bool p) { read_pending = p; }

    void tick() {
        poll_count_++;
        if (poll_count_ >= 1000) poll_count_ = 0;
    }

    uint32_t poll_count() const { return poll_count_; }
    uint32_t connected_count() const { return connected_count_; }

    void reset() {
        device_connected = false;
        endpoints_enabled = false;
        read_pending = false;
    }

private:
    DeviceState() : poll_count_(0), connected_count_(0) {}

    uint32_t poll_count_;
    uint32_t connected_count_;
};

}
}
#endif

#endif