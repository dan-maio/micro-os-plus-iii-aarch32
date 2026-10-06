/**
 * @file usb_system.hpp
 * @brief USB system-level operations and state management.
 */

#ifndef USB_SYSTEM_HPP_
#define USB_SYSTEM_HPP_

#include <stdint.h>
#include "usb_device_state.hpp"
#include "usb_interrupt_handler.hpp"

namespace rk3506 {
namespace usb {

class Device {
public:
    void set_connected(bool c) { connected_ = c; }
    bool is_connected() const { return connected_; }
private:
    bool connected_ = false;
};

class System {
public:
    static System& instance() {
        static System sys;
        return sys;
    }

    [[noreturn]] void run() {
        while (true) {
            handle_interrupt();
            poll();
        }
    }

    void handle_interrupt() {
        InterruptHandler::instance().handle_interrupt(0);
    }

    void poll() {
        InterruptHandler::instance().tick();

        const bool conn = DeviceState::instance().is_connected();
        const bool en = DeviceState::instance().are_enabled();

        if (conn && en) {
            if (!read_queue_attempted_) {
                if (poll_count_ > 500) {
                    InterruptHandler::instance().start_read_queue();
                    read_queue_attempted_ = 1;
                    last_read_queue_attempt_ = poll_count_;
                    cdc_.set_connected(true);
                    vdev_.set_connected(true);
                }
            } else if ((poll_count_ - last_read_queue_attempt_ > 1000)) {
                if (!DeviceState::instance().is_pending()) {
                    read_queue_retry_count_++;
                    if (read_queue_retry_count_ <= 5) {
                        InterruptHandler::instance().start_read_queue();
                        last_read_queue_attempt_ = poll_count_;
                    }
                }
            }
        }

        poll_count_++;
        if (poll_count_ >= 1000) {
            poll_count_ = 0;
        }
        total_poll_count_++;
    }

    Device& cdc() { return cdc_; }
    Device& vendor() { return vdev_; }

private:
    System() = default;

    Device cdc_;
    Device vdev_;

    uint32_t poll_count_ = 0;
    uint32_t read_queue_attempted_ = 0;
    uint32_t read_queue_retry_count_ = 0;
    uint32_t last_read_queue_attempt_ = 0;
    uint32_t total_poll_count_ = 0;
};

}

}

#endif