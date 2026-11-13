// wifi_app.hpp - what the Wi-Fi tests share: the scripted bring-up of the
// radio and the link, and a thread that owns the SD card and saves files on
// request.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rpiz2w::wifi_app
{
  struct bring_up_result
  {
    bool radio{ false };    ///< the chip answered and its firmware runs
    bool scanned{ false };  ///< the scan saw at least one network
    bool joined{ false };   ///< associated (and keyed, on a secured network)
    bool address{ false };  ///< DHCP lease
  };

  /// Start the radio and lwIP, scan, join `ssid` and wait for an address.
  /// Starts the SD thread once the radio is up (both program GPIO function
  /// registers). Prints every step. Must run on the thread that owns the
  /// network stack.
  bring_up_result bring_up (const char* ssid, const char* passphrase) noexcept;

  /// Why the bring-up stopped. For the RESULT line: it is the only text
  /// that reaches the runner when nobody reads the console.
  [[nodiscard]] const char* failure_detail () noexcept;

  /// Service the stack for `ms` milliseconds (2 ms cadence).
  void serve_for (std::uint32_t ms) noexcept;

  // ---- the SD card thread ------------------------------------------------
  // One thread owns the card and FatFs. The network thread never blocks on
  // the card: it posts a request and keeps polling until it is done.

  /// Create the SD thread (bring_up() does). It initialises the card on the
  /// SDHOST controller and mounts the FAT volume of the boot card; files go
  /// to its "tests" folder.
  void saver_start () noexcept;

  /// True once the card is mounted (or has failed to: see saver_failed()).
  [[nodiscard]] bool saver_ready () noexcept;
  [[nodiscard]] bool saver_failed () noexcept;

  /// Ask for `size` bytes at `data` to be written to tests/<leaf> (8.3 name)
  /// and read back for comparison. `data` must stay valid until the request
  /// completes. False when a request is already running.
  bool saver_request (const char* leaf, const void* data,
                      std::uint32_t size) noexcept;

  [[nodiscard]] bool saver_busy () noexcept;

  /// Result of the last completed request: written and verified.
  [[nodiscard]] bool saver_ok () noexcept;

  /// Post a request and service the network until it completes.
  bool save_and_wait (const char* leaf, const void* data, std::uint32_t size,
                      std::uint32_t timeout_ms = 30000) noexcept;

  /// Formatted text appended to a RAM log, which a test saves at the end.
  void log (const char* format, ...) noexcept
      __attribute__ ((format (printf, 1, 2)));
  [[nodiscard]] const char* log_text () noexcept;
  [[nodiscard]] std::uint32_t log_size () noexcept;
} // namespace rpiz2w::wifi_app
