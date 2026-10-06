/**
 * @file  sd_dwmmc.cpp
 * @brief `sd::SdCard` -- the eight methods, forwarding to sdcard::SdMmc.
 *
 * There is no controller logic in this file on purpose. Everything that
 * touches the RK3506 lives in sdmmc.cpp, exactly as the predecessor left it.
 */

#include <sd_dwmmc.hpp>

namespace sd
{
  bool
  SdCard::init ()
  {
    if (hw_.init ())
      {
        result_ = Result::ok;
        err_ = "no error";
        return true;
      }

    // The driver records how far bring-up got; step_name() is the most
    // specific thing anyone can say about the failure, so it is what the
    // caller gets.
    err_ = sdcard::step_name (hw_.step ());
    // Step is ordered: anything that did not get past CMD0 means the card
    // never answered at all, which is what "no card" means here. Past that
    // the card is talking and the failure is a protocol/timing one.
    result_ = static_cast<unsigned> (hw_.step ())
                      <= static_cast<unsigned> (sdcard::Step::Cmd0)
                  ? Result::err_no_card
                  : Result::err_timeout;
    return false;
  }

  bool
  SdCard::range_ok (std::uint32_t lba, std::uint32_t count)
  {
    if (!hw_.present ())
      {
        result_ = Result::err_no_card;
        err_ = "no card";
        return false;
      }
    const std::uint32_t total = sector_count ();
    if (count == 0 || lba > total || count > total - lba)
      {
        result_ = Result::err_range;
        err_ = "LBA range beyond end of card";
        return false;
      }
    return true;
  }

  bool
  SdCard::read_sectors (std::uint32_t lba, std::uint32_t count, void* dst)
  {
    if (!range_ok (lba, count))
      {
        return false;
      }
    if (!hw_.read_blocks (lba, dst, count))
      {
        result_ = Result::err_timeout;
        err_ = "read failed";
        return false;
      }
    result_ = Result::ok;
    err_ = "no error";
    return true;
  }

  bool
  SdCard::write_sectors (std::uint32_t lba, std::uint32_t count,
                            const void* src)
  {
    if (!range_ok (lba, count))
      {
        return false;
      }
    if (!hw_.write_blocks (lba, src, count))
      {
        result_ = Result::err_timeout;
        err_ = "write failed";
        return false;
      }
    result_ = Result::ok;
    err_ = "no error";
    return true;
  }

} // namespace sd
