/**
 * @file  led.hpp
 * @brief User LED helper for the Luckfox Lyra B (RK3506).
 *
 * The Lyra's user LED is GPIO1_A0. Reaching it takes more than a data
 * register write: the GPIO1 controller, its debounce clock and the GPIO1_IOC
 * pinmux block all come out of reset gated, so init() ungates and de-resets
 * them, muxes A0 to plain GPIO, and only then drives the pin. Each of those
 * registers is a Rockchip write-mask register -- the upper half-word says
 * which bits the lower half-word is allowed to change -- so none of this is
 * a read-modify-write and none of it needs a lock.
 *
 * This sequence was copied verbatim in every one of the predecessor
 * project's thirteen test directories. It lives here once instead.
 *
 * Under QEMU (`-M virt`) none of these blocks exist and none of these
 * addresses are mapped, so the whole thing degrades to no-ops rather than
 * taking a data abort on the first blink.
 */
#pragma once

#include <cstdint>
#include "rk3506.hpp"

namespace led {

#if defined(QEMU_BUILD)

inline void init() noexcept {}
inline void on() noexcept {}
inline void off() noexcept {}

#else

inline void init() noexcept {
    using namespace rk3506;
    cru::CLKGATE_CON22::clear_bits(0x0002);   // ungate GPIO1_IOC
    cru::CLKGATE_CON03::clear_bits(0x0300);   // ungate GPIO1 ctrl + debounce
    cru::SOFTRST_CON22::clear_bits(0x0002);   // release GPIO1_IOC reset
    cru::SOFTRST_CON03::clear_bits(0x0300);   // release GPIO1 resets
    gpio1_ioc::GPIO1A_IOMUX_SEL0::write(0x000F, 0x0000);  // A0 -> GPIO
    gpio1::SWPORT_DDR_L::write(0x0001, 0x0001);           // A0 -> output
}

inline void on() noexcept {
    rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0001);
}

inline void off() noexcept {
    rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0000);
}

#endif // QEMU_BUILD

inline void set(bool onoff) noexcept { onoff ? on() : off(); }

} // namespace led
