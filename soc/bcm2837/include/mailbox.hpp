/**
 * @file  mailbox.hpp
 * @brief VideoCore property-channel (mailbox 8) client for BCM2837.
 *
 * The ARM asks the VideoCore firmware for things — power domains, clocks,
 * board info — by handing it a tag buffer through mailbox channel 8. The one
 * user here is powering up the USB block before the DWC2 driver touches a
 * register; without it the core can read back as zeroes or keep its PHY down,
 * depending on what the firmware left behind.
 *
 * Register offsets match the in-repo, proven Pi 3 B client in led.hpp:
 * read = 0x00, status = 0x18 (bit31 FULL / bit30 EMPTY, one register for both
 * directions), write = 0x20. Using a separate "0x38" status was the bug that
 * made every hardware bring-up report `USB power domain: FAILED`.
 *
 * Buffer layout (all u32, little-endian):
 *
 *     [0] total size in bytes (including this word and the end tag)
 *     [1] code: 0 = request; 0x80000000 = success on return
 *     [2] tag id
 *     [3] value buffer size in bytes
 *     [4] value length in bytes (| 0x80000000 on return)
 *     [5..] value words
 *     [n] 0 = end tag
 *
 * QEMU's raspi machines model this channel (hw/misc/bcm2835_property.c), so
 * property_call() is exercised under the emulator as well as on silicon.
 */
#pragma once

#include <cstdint>

namespace mailbox
{

/// Response code meaning the firmware accepted and processed the request.
inline constexpr std::uint32_t kRespSuccess = 0x8000'0000u;

/// Property tag: set a power domain's state.
inline constexpr std::uint32_t kTagSetPowerState = 0x0002'8001u;

/// Power-domain device id of the USB HCD block.
inline constexpr std::uint32_t kPowerDeviceUsb = 3u;

/**
 * Exchange a property buffer with the VideoCore.
 *
 * `buf` must be 16-byte aligned and already contain a well-formed request
 * (size, code 0, tags, end tag). On return the buffer holds the response.
 *
 * Returns true when the firmware replied with kRespSuccess.
 */
bool property_call (std::uint32_t* buf) noexcept;

/**
 * Power a device domain on or off and wait for it to settle.
 *
 * Returns true when the firmware reported success AND the returned state
 * matches what was asked for — a firmware that accepts the tag but leaves the
 * domain off returns false rather than a silent success.
 */
bool set_power_state (std::uint32_t device_id, bool on) noexcept;

/// Diagnostics from the last set_power_state(), for hardware bring-up logs.
std::uint32_t last_code () noexcept;   ///< response code word (buf[1])
std::uint32_t last_state () noexcept;  ///< returned power state word (buf[6])
bool last_used_alias () noexcept;      ///< true if the 0xC0000000 bus alias was used

} // namespace mailbox
