/**
 * @file usb_interrupt_handler.hpp
 * @brief USB interrupt handling and callback registration.
 */

#ifndef USB_INTERRUPT_HANDLER_HPP_
#define USB_INTERRUPT_HANDLER_HPP_

#include <stdint.h>

namespace rk3506 {
namespace usb {

class InterruptHandler {
public:
    static InterruptHandler& instance() {
        static InterruptHandler handler;
        return handler;
    }

    void handle_interrupt(int index = 0);

    void start_read_queue();

    void tick();

    bool should_start_queue(bool connected, bool enabled) const {
        return connected && enabled;
    }

    bool should_retry_queue(bool connected, bool enabled, bool pending, uint32_t polls_since_start) const {
        return connected && enabled && !pending && polls_since_start > 1000;
    }

private:
    InterruptHandler() = default;
};

}
}

#endif