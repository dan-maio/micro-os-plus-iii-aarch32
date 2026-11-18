/**
 * @file sink.hpp
 * @brief Storage backends for usb_test: flatfs-on-SD, and a RAM fallback.
 *
 * The sink knows nothing about USB. `usb_dwc2` knows nothing about files. The
 * command loop in main.cpp is the only place the two meet.
 *
 * Neither sink writes the console: the core-0 USB service thread posts the
 * received bytes to the UART and semihosting writer threads, which are the
 * only writers of those consoles (uart.hpp has no locking).
 */
#pragma once

#include <cstdint>

#include "flatfs.hpp"

namespace usbtest
{

class Sink
{
public:
  enum class Result
  {
    ok = 0,
    not_found,
    no_space,
    io_error,
    too_large,
    bad_name
  };

  Sink () = default;
  virtual ~Sink () = default;
  Sink (const Sink&) = delete;
  Sink& operator= (const Sink&) = delete;

  virtual const char* name () const noexcept = 0;
  virtual Result put (const char* n, const void* data, std::uint32_t len) = 0;
  virtual Result get (const char* n, void* buf, std::uint32_t cap,
                      std::uint32_t& out_len) = 0;
  virtual Result list (flatfs::FileVisitor v, void* ctx) = 0;
};

/// The fallback file size when no card is present.
inline constexpr std::uint32_t kRamFileMax = 128u * 1024u;

/// flatfs on the SD card. All calls happen on the core-0 USB service thread.
class FlatFsSink : public Sink
{
public:
  explicit FlatFsSink (flatfs::FlatFs& fs) noexcept : fs_ (fs) {}
  const char* name () const noexcept override { return "FlatFsSink"; }
  Result put (const char* n, const void* data, std::uint32_t len) override;
  Result get (const char* n, void* buf, std::uint32_t cap,
              std::uint32_t& out_len) override;
  Result list (flatfs::FileVisitor v, void* ctx) override;

private:
  flatfs::FlatFs& fs_;
};

/// No card: keep the most recent file in RAM so PUT then GET still round-trips.
/// The console stream is produced by the writer threads from the same bytes.
#if defined(HW_BUILD)
/// FAT32 on the real boot card.
///
/// On hardware the microSD is the BOOT card: an MBR plus a FAT32 partition
/// carrying config.txt and kernel7.img / kernel8.img at its root. This sink
/// therefore MOUNTS the existing volume and never formats it, and every file
/// it writes lives under "tests/", so nothing at the FAT root is ever touched.
/// `flatfs` cannot be used here at all: its superblock is LBA 0, which on a
/// boot card is the MBR.
///
/// FatFs is built with FF_USE_LFN = 0 (there is no ffunicode.c in the tree),
/// so the volume only accepts 8.3 names. Host names are folded to 8.3 by
/// to_83(); put/get/list all apply it, so a GET issued with the original host
/// name still finds the file.
class FatFsSink : public Sink
{
public:
  const char* name () const noexcept override
  {
    return "FatFsSink (FAT32 tests/)";
  }

  /// Mount the existing volume and make sure tests/ exists. Call once, from
  /// the SD thread, before any put/get/list. The card must already be bound
  /// with fatfshw::bind_card().
  bool mount () noexcept;

  Result put (const char* n, const void* data, std::uint32_t len) override;
  Result get (const char* n, void* buf, std::uint32_t cap,
              std::uint32_t& out_len) override;
  Result list (flatfs::FileVisitor v, void* ctx) override;

  /// Fold a host name to the 8.3 name it is stored under. Exposed so the
  /// console can print the mapping instead of silently renaming the file.
  static void to_83 (const char* n, char out[13]) noexcept;
};
#endif // HW_BUILD

class RamUartSink : public Sink
{
public:
  const char* name () const noexcept override { return "RamUartSink"; }
  Result put (const char* n, const void* data, std::uint32_t len) override;
  Result get (const char* n, void* buf, std::uint32_t cap,
              std::uint32_t& out_len) override;
  Result list (flatfs::FileVisitor v, void* ctx) override;

private:
  static std::uint8_t buf_[kRamFileMax];
  std::uint32_t len_ = 0u;
  char name_[flatfs::kNameMax + 1] = {};
  bool has_ = false;
  bool truncated_ = false;
};

} // namespace usbtest
