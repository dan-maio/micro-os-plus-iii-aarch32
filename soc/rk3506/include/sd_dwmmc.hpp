/**
 * @file  sd_dwmmc.hpp
 * @brief The RK3506 (Luckfox Lyra B) SD back end behind `sd::SdCard`.
 *
 * The driver itself is sdmmc.hpp/sdmmc.cpp, carried over unchanged from the
 * predecessor's cortex-a7 project, where it was brought up and tuned against
 * the real card: CRU gating and the 24 MHz cclk_src_sdmmc select, the GPIO3_A
 * pinmux, the DesignWare reset/FIFO sequence, the sample and drive phases, and
 * the PIO transfer path. None of that is re-derived here -- inventing a second
 * bring-up sequence for silicon that already has a working one is how the
 * phases and the clock source get lost.
 *
 * What this file adds is only the shape the rest of the workspace expects:
 * `sd::SdCard`'s eight methods, forwarding to `sdcard::SdMmc`. flatfs, FatFs'
 * diskio and the tests compile against it unchanged, and nothing a Raspberry
 * Pi builds is touched by its existence.
 */
#pragma once

#include <cstdint>

#include <sdmmc.hpp>

namespace sd
{
  // 512-byte logical sector. Declared here as well as in sd.hpp's SDHCI
  // branch, because exactly one of the two branches is ever compiled.
  inline constexpr std::uint32_t kSectorSize = 512u;

  class SdCard
  {
  public:
    // Same names as the SDHCI back end's, so a caller that switches boards
    // does not switch vocabulary. The DW-MMC driver reports a bring-up
    // `Step` rather than a category, so the mapping is deliberately coarse:
    // last_error() carries the detail, as a string, from step_name().
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

    bool init ();

    [[nodiscard]] std::uint32_t sector_count () const noexcept
    {
      return static_cast<std::uint32_t> (hw_.capacity_bytes ()
                                         / sdcard::kBlockSize);
    }

    [[nodiscard]] std::uint64_t capacity_bytes () const noexcept
    {
      return hw_.capacity_bytes ();
    }

    [[nodiscard]] bool high_capacity () const noexcept
    {
      return hw_.high_capacity ();
    }

    [[nodiscard]] bool ready () const noexcept { return hw_.present (); }

    [[nodiscard]] const char* last_error () const noexcept { return err_; }

    [[nodiscard]] Result last_result () const noexcept { return result_; }

    bool read_sectors (std::uint32_t lba, std::uint32_t count, void* dst);

    bool write_sectors (std::uint32_t lba, std::uint32_t count,
                        const void* src);

    /// The driver underneath, for a board that wants its diagnostics.
    [[nodiscard]] sdcard::SdMmc& hw () noexcept { return hw_; }

  private:
    bool range_ok (std::uint32_t lba, std::uint32_t count);

    sdcard::SdMmc hw_ {};
    Result result_ = Result::ok;
    const char* err_ = "no error";
  };

} // namespace sd
