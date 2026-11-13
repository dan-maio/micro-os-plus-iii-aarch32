/**
 * @file  sd_sdhost.hpp
 * @brief SD card on the BCM2835 "SDHOST" controller (0x3F202000), polled.
 *
 * The BCM2837 has two SD controllers. The Arasan SDHCI (sd.hpp's default
 * back end) can reach the card slot, but on the Raspberry Pi Zero 2 W and the
 * Pi 3 it is also the only one wired to the Wi-Fi chip. A build that uses the
 * radio gives the Arasan to the radio and reads the card through this one,
 * which is what the firmware and Linux do on these boards.
 *
 * Selected with SD_BACKEND_SDHOST. The same `sd::SdCard` as the other back
 * ends: flatfs, FatFs' diskio and the tests name only this interface.
 *
 * Pins: GPIO48-53 in ALT0 (the Arasan uses the same pins in ALT3). init()
 * routes them.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sd
{
  inline constexpr std::uint32_t kSectorSize = 512u;

  class SdCard
  {
  public:
    enum class Result
    {
      ok,
      err_no_card,
      err_timeout,
      err_crc,
      err_cmd,
      err_voltage,
      err_unsupported,
      err_range,
    };

    SdCard () = default;
    SdCard (const SdCard&) = delete;
    SdCard& operator= (const SdCard&) = delete;

    /// Reset the controller and run the SD identification sequence.
    bool init ();

    [[nodiscard]] std::uint32_t
    sector_count () const noexcept
    {
      return sectors_;
    }
    [[nodiscard]] std::uint64_t
    capacity_bytes () const noexcept
    {
      return static_cast<std::uint64_t> (sectors_) * kSectorSize;
    }
    [[nodiscard]] bool
    high_capacity () const noexcept
    {
      return hc_;
    }
    [[nodiscard]] bool
    ready () const noexcept
    {
      return ready_;
    }
    [[nodiscard]] const char*
    last_error () const noexcept
    {
      return err_;
    }
    [[nodiscard]] Result
    last_result () const noexcept
    {
      return result_;
    }

    bool read_sectors (std::uint32_t lba, std::uint32_t count, void* dst);
    bool write_sectors (std::uint32_t lba, std::uint32_t count, const void* src);

  private:
    bool command (std::uint32_t index, std::uint32_t argument,
                  std::uint32_t flags, bool ignore_crc = false);
    bool app_command (std::uint32_t index, std::uint32_t argument,
                      std::uint32_t flags, bool ignore_crc = false);
    bool wait_ready ();
    bool wait_fsm_idle (bool read);
    bool block (bool read, std::uint32_t lba, std::uint8_t* data);
    bool fail (Result result, const char* message);

    bool ready_ = false;
    bool hc_ = false;
    std::uint32_t sectors_ = 0;
    std::uint32_t rca_ = 0;
    Result result_ = Result::ok;
    const char* err_ = "no error";
  };
} // namespace sd
