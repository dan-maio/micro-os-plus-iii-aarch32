// sdio.hpp - an SDIO host on the BCM2837's Arasan SDHCI (0x3F300000).
//
// On the Raspberry Pi Zero 2 W the Wi-Fi chip is wired to this controller
// through GPIO34-39. The SD card slot can be reached by the same controller
// (GPIO48-53) or by the second one, SDHOST; while the radio is in use the
// Arasan belongs to the radio.
//
//   start()      route the pins, power the chip, run the SDIO enumeration
//                (CMD5, CMD3, CMD7), switch to 4 bits and 25 MHz
//   cmd52        one register byte            (IO_RW_DIRECT)
//   cmd53        a run of bytes or of blocks  (IO_RW_EXTENDED), polled
#pragma once

#include <cstddef>
#include <cstdint>

namespace rpiz2w::sdio
{
  /// Bring the bus up. False when no SDIO card answers.
  [[nodiscard]] bool start () noexcept;

  /// Read one byte of function `fn` at `address`.
  [[nodiscard]] bool read8 (unsigned fn, std::uint32_t address,
                            std::uint8_t& value) noexcept;
  /// Write one byte of function `fn` at `address`.
  [[nodiscard]] bool write8 (unsigned fn, std::uint32_t address,
                             std::uint8_t value) noexcept;

  /// Transfer `size` bytes at `address` of function `fn`. Up to one block is
  /// sent in byte mode; more must be a multiple of the function's block size
  /// and goes in block mode. `data` must be 4-byte aligned.
  [[nodiscard]] bool read (unsigned fn, std::uint32_t address, void* data,
                           std::size_t size) noexcept;
  [[nodiscard]] bool write (unsigned fn, std::uint32_t address,
                            const void* data, std::size_t size) noexcept;

  /// Block size used for block-mode transfers (the same for every function).
  inline constexpr std::size_t block_size = 64;

  /// What the last failed operation was, for a bring-up log.
  [[nodiscard]] const char* last_error () noexcept;
  /// Operating-conditions register from CMD5, and the relative address.
  [[nodiscard]] std::uint32_t ocr () noexcept;
  [[nodiscard]] std::uint32_t rca () noexcept;
} // namespace rpiz2w::sdio
