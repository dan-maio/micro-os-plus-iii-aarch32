/**
 * @file  led.hpp
 * @brief User LED helper for the Raspberry Pi Zero 2W / Pi 3 B (AArch32).
 *
 * Board is selected at build time:
 *
 *   Pi Zero 2 W  (default, `make` / `make BOARD=zero2w`):
 *     Single GPIO "user LED" via GPFSEL/GPSET/GPCLR. The pin is compile-time
 *     selectable with -DLED_PIN=<n> (default GPIO16, header pin 36). Wire an
 *     LED (+ series resistor) from that pin to ground. Under QEMU raspi2b
 *     there is no LED — the register writes are harmless no-ops.
 *
 *   Pi 3 B  (`make BOARD=rpi3b`, -DBOARD_RPI3B):
 *     The onboard ACT (green) LED is NOT a plain SoC GPIO on the Pi 3 B: it is
 *     the VideoCore firmware expander GPIO 130 (STATUS_LED, active high) and
 *     is driven through the ARM<->VideoCore mailbox property channel
 *     (property tag 0x00038041 SET_GPIO_STATE). All waits are bounded so a
 *     run without firmware (e.g. QEMU) degrades to no-ops instead of hanging.
 */
#pragma once

#include <cstdint>
#include <atomic>
#include "bcm2837.hpp"

#ifdef BOARD_RPI3B

namespace led {

namespace detail {

// ARM <-> VideoCore legacy mailbox (property channel = 8).
constexpr std::uint32_t kMboxBase     = 0x3F00B880u;
constexpr std::uint32_t kMboxRead     = kMboxBase + 0x00u;
constexpr std::uint32_t kMboxStatus   = kMboxBase + 0x18u;
constexpr std::uint32_t kMboxWrite    = kMboxBase + 0x20u;
constexpr std::uint32_t kStatusEmpty  = 1u << 30;
constexpr std::uint32_t kStatusFull   = 1u << 31;
constexpr std::uint32_t kChannelProp  = 8u;
constexpr std::uint32_t kRespOk       = 0x80000000u;
constexpr std::uint32_t kTagSetGpio   = 0x00038041u; // SET_GPIO_STATE
constexpr std::uint32_t kActLed       = 130u;        // expander STATUS_LED

constexpr std::uint32_t kMaxSpin = 0x400000u;        // bounded wait

inline void cache_clean(const volatile void* p, std::uint32_t bytes) noexcept {
    // DCCMVAC: clean data cache line by VA to point of coherency.
    const std::uint32_t line = 64u; // Cortex-A53
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p) & ~(line - 1u);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(p) + bytes;
    while (a < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
        a += line;
    }
    __asm__ volatile("dsb" ::: "memory");
}

inline void cache_invalidate(const volatile void* p, std::uint32_t bytes) noexcept {
    // DCIMVAC: invalidate data cache line by VA to point of coherency.
    const std::uint32_t line = 64u;
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p) & ~(line - 1u);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(p) + bytes;
    while (a < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
        a += line;
    }
    __asm__ volatile("dsb" ::: "memory");
}

inline bool set_state(std::uint32_t pin, bool on) noexcept {
    static std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
    while (s_lock.test_and_set(std::memory_order_acquire)) {
        __asm__ volatile("yield" ::: "memory");
    }

    alignas(16) volatile std::uint32_t buffer[8];
    buffer[0] = 8u * 4u;              // total buffer size (bytes)
    buffer[1] = 0u;                   // request code
    buffer[2] = kTagSetGpio;
    buffer[3] = 8u;                   // value buffer size
    buffer[4] = 8u;                   // request size
    buffer[5] = pin;
    buffer[6] = on ? 1u : 0u;
    buffer[7] = 0u;                   // end tag

    cache_clean(&buffer[0], sizeof(buffer));

    using bcm::mmio_read;
    using bcm::mmio_write;
    const std::uint32_t addr = reinterpret_cast<std::uint32_t>(const_cast<std::uint32_t*>(&buffer[0]));

    // Send the request (bounded wait for the write buffer to empty).
    std::uint32_t spins = kMaxSpin;
    while (mmio_read(kMboxStatus) & kStatusFull) {
        if (--spins == 0u) {
            s_lock.clear(std::memory_order_release);
            return false;
        }
    }
    mmio_write(kMboxWrite, (addr & 0xFFFFFFF0u) | kChannelProp);

    // Wait for the property-channel response.
    std::uint32_t response = 0u;
    do {
        spins = kMaxSpin;
        while (mmio_read(kMboxStatus) & kStatusEmpty) {
            if (--spins == 0u) {
                s_lock.clear(std::memory_order_release);
                return false;
            }
        }
        response = mmio_read(kMboxRead);
    } while ((response & 0xFu) != kChannelProp);

    cache_invalidate(&buffer[0], sizeof(buffer));
    const bool ok = (buffer[1] & 0x80000000u) == kRespOk;
    s_lock.clear(std::memory_order_release);
    return ok;
}

} // namespace detail

inline void init() noexcept { detail::set_state(detail::kActLed, false); }
inline void on()    noexcept { detail::set_state(detail::kActLed, true); }
inline void off()   noexcept { detail::set_state(detail::kActLed, false); }
inline void set(bool onoff) noexcept { onoff ? on() : off(); }

} // namespace led

#else  // Pi Zero 2 W: direct GPIO user LED.

#ifndef LED_PIN
#define LED_PIN 16
#endif

namespace led {

inline void init() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    const std::uint32_t reg = gpio::GPFSEL0 + (pin / 10) * 4u;
    const unsigned shift = (pin % 10) * 3u;
    std::uint32_t v = mmio_read(reg);
    v &= ~(0x7u << shift);
    v |=  (0x1u << shift);           // 0b001 = output
    mmio_write(reg, v);
}

inline void on() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    mmio_write(gpio::GPSET0 + (pin / 32) * 4u, 1u << (pin % 32));
}

inline void off() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    mmio_write(gpio::GPCLR0 + (pin / 32) * 4u, 1u << (pin % 32));
}

inline void set(bool onoff) noexcept { onoff ? on() : off(); }

} // namespace led

#endif // BOARD_RPI3B
