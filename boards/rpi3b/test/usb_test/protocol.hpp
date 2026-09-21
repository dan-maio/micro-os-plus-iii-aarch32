/**
 * @file protocol.hpp
 * @brief usb_test wire protocol: 32-byte command, 16-byte reply, CRC-32.
 *
 * Shared with the host script by construction (host_xfer.py packs the same
 * little-endian layout with struct.pack('<IBBH20sI')).
 */
#pragma once

#include <cstdint>

namespace usbtest
{

inline constexpr std::uint32_t kCmdMagic = 0x58534F55u; // 'UOSX' little-endian
inline constexpr std::uint32_t kRspMagic = 0x52534F55u; // 'UOSR'

inline constexpr std::uint32_t kNameField = 20u; // matches flatfs::kNameMax + 1

enum class Op : std::uint8_t
{
  ping = 1,
  put = 2,
  get = 3,
  list = 4
};

struct [[gnu::packed]] Command
{
  std::uint32_t magic;
  std::uint8_t op;
  std::uint8_t flags; // bit0 on a ping = "terminate the run"
  std::uint16_t reserved;
  char name[kNameField]; // NUL-padded
  std::uint32_t length;  // payload bytes following (put), else 0
};
static_assert (sizeof (Command) == 32, "Command must be 32 bytes on the wire");

struct [[gnu::packed]] Reply
{
  std::uint32_t magic;
  std::uint32_t status; // 0 = ok, else a Sink::Result code
  std::uint32_t length;
  std::uint32_t crc32; // of the payload that follows
};
static_assert (sizeof (Reply) == 16, "Reply must be 16 bytes on the wire");

/// IEEE 802.3 CRC-32, reflected, init/final 0xFFFFFFFF — the same polynomial
/// Python's zlib.crc32 uses, so the host needs no private implementation.
std::uint32_t crc32 (const void* data, std::uint32_t len) noexcept;

} // namespace usbtest
