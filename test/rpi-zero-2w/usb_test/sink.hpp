/**
 * @file sink.hpp
 * @brief Storage backends for usb_test: the SD card through µOS++ POSIX I/O,
 *        and a RAM fallback.
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

namespace usbtest
{

/// The longest file name: the protocol's name field (protocol.hpp), less the
/// NUL.
inline constexpr std::uint32_t kNameMax = 19u;

/// One file of a listing.
struct FileEntry
{
  char name[kNameMax + 1]; // NUL-terminated
  std::uint32_t start_block = 0;
  std::uint32_t size_bytes = 0;
  bool valid = false;
};

using FileVisitor = bool (*) (const FileEntry& file, void* ctx);

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
  virtual Result list (FileVisitor v, void* ctx) = 0;
};

/// The fallback file size when no card is present.
inline constexpr std::uint32_t kRamFileMax = 128u * 1024u;

/// The SD card, through µOS++ POSIX I/O: the card's FAT volume mounted at
/// /sd/ (the posix-io block device /dev/sd with Chan FatFs,
/// uos_test_storage_start()). On hardware it is the BOOT card, never
/// formatted; under QEMU an MBR + FAT32 image. Every file lives in
/// /sd/tests/, so nothing at the volume's root is ever touched. Long names:
/// a file keeps its host name. All calls happen on the SD thread.
class PosixSink : public Sink
{
public:
  const char* name () const noexcept override
  {
    return "PosixSink (/sd/tests)";
  }
  Result put (const char* n, const void* data, std::uint32_t len) override;
  Result get (const char* n, void* buf, std::uint32_t cap,
              std::uint32_t& out_len) override;
  Result list (FileVisitor v, void* ctx) override;
};

/// No card: keep the most recent file in RAM so PUT then GET still round-trips.
/// The console stream is produced by the writer threads from the same bytes.
class RamUartSink : public Sink
{
public:
  const char* name () const noexcept override { return "RamUartSink"; }
  Result put (const char* n, const void* data, std::uint32_t len) override;
  Result get (const char* n, void* buf, std::uint32_t cap,
              std::uint32_t& out_len) override;
  Result list (FileVisitor v, void* ctx) override;

private:
  static std::uint8_t buf_[kRamFileMax];
  std::uint32_t len_ = 0u;
  char name_[kNameMax + 1] = {};
  bool has_ = false;
  bool truncated_ = false;
};

} // namespace usbtest
