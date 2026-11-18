// wifi_app.cpp - the shared bring-up script and the SD card thread.
#include "wifi_app.hpp"

#include "wlan.hpp"
#include "net.hpp"
#include "platform.hpp"

#include <cmsis-plus/rtos/os.h>

#include <cmsis-plus/posix-io/file-system.h>
#include <cmsis-plus/posix-io/io.h>
#include <cmsis-plus/posix/sys/statvfs.h>
#include <uos-test-storage.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace os::rtos;

namespace rpiz2w::wifi_app
{
  namespace
  {
    // ------------------------------------------------------------ RAM log
    char g_log[8192];
    std::uint32_t g_log_size = 0;

    char g_detail[160] = "";

    // ---------------------------------------------------------- SD thread
    alignas (16) std::uint8_t g_sd_stack[64 * 1024];
    semaphore_binary g_sd_work{ "sd-work", 0 };

    volatile bool g_sd_ready = false;
    volatile bool g_sd_failed = false;
    volatile bool g_sd_busy = false;
    volatile bool g_sd_ok = false;

    const char* g_req_leaf = nullptr;
    const void* g_req_data = nullptr;
    std::uint32_t g_req_size = 0;

    // Read-back buffer for the comparison, one cluster-friendly piece at a
    // time so a multi-megabyte file needs no second copy in RAM.
    std::uint8_t g_verify[64 * 1024];

    // The files go to /sd/tests/, through µOS++ POSIX I/O, on the boot
    // card's FAT volume (the posix-io block device /dev/sd with Chan FatFs,
    // mounted by uos_test_storage_start(); never formatted). The card is on
    // SDHOST (micro-os-plus::devices-sdhost-posix): the Arasan is the radio's.
    bool
    store_and_verify (const char* leaf, const void* data, std::uint32_t size)
    {
      char path[64];
      std::snprintf (path, sizeof (path), "/sd/tests/%s", leaf);
      os::posix::io* f = os::posix::open (path, O_WRONLY | O_CREAT | O_TRUNC);
      bool ok = (f != nullptr);
      if (ok)
        {
          ok = f->write (data, size) == static_cast<ssize_t> (size);
          ok = (f->close () == 0) && ok;
        }
      if (!ok)
        {
          kprintf ("[SD] write %s failed (errno %d)\n", path, errno);
          return false;
        }
      struct stat st;
      if (os::posix::stat (path, &st) != 0
          || static_cast<std::uint32_t> (st.st_size) != size)
        {
          kprintf ("[SD] %s is not %u bytes\n", path,
                   static_cast<unsigned> (size));
          return false;
        }
      f = os::posix::open (path, O_RDONLY);
      if (f == nullptr)
        {
          kprintf ("[SD] %s cannot be opened (errno %d)\n", path, errno);
          return false;
        }
      const auto* expected = static_cast<const std::uint8_t*> (data);
      for (std::uint32_t at = 0; ok && at < size;)
        {
          const std::uint32_t piece
              = size - at < sizeof (g_verify) ? size - at : sizeof (g_verify);
          if (f->read (g_verify, piece) != static_cast<ssize_t> (piece)
              || std::memcmp (g_verify, expected + at, piece) != 0)
            {
              kprintf ("[SD] %s differs at offset %u\n", path,
                       static_cast<unsigned> (at));
              ok = false;
            }
          at += piece;
        }
      f->close ();
      if (ok)
        {
          kprintf ("[SD] saved %s, %u bytes, read back and verified\n", path,
                   static_cast<unsigned> (size));
        }
      return ok;
    }

    void*
    sd_func (void*)
    {
      // /dev/sd's first FAT partition mounted at /sd/, /sd/tests/ created.
      uos_test_storage_start ();
      struct statvfs vfs;
      if (os::posix::statvfs ("/sd/", &vfs) != 0)
        {
          kprintf ("[SD] no FAT volume mounted at /sd/\n");
          g_sd_failed = true;
        }
      else
        {
          kprintf ("[SD] FAT volume mounted at /sd/, files go to "
                   "/sd/tests/\n");
        }
      g_sd_ready = true;

      for (;;)
        {
          g_sd_work.wait ();
          g_sd_ok = !g_sd_failed
                    && store_and_verify (g_req_leaf, g_req_data, g_req_size);
          g_sd_busy = false;
        }
      return nullptr;
    }
  } // namespace

  void
  log (const char* format, ...) noexcept
  {
    if (g_log_size >= sizeof (g_log) - 1)
      {
        return;
      }
    std::va_list args;
    va_start (args, format);
    const int n = std::vsnprintf (g_log + g_log_size,
                                  sizeof (g_log) - g_log_size, format, args);
    va_end (args);
    if (n > 0)
      {
        g_log_size += static_cast<std::uint32_t> (n);
        if (g_log_size > sizeof (g_log) - 1)
          {
            g_log_size = sizeof (g_log) - 1;
          }
      }
  }

  const char*
  log_text () noexcept
  {
    return g_log;
  }

  std::uint32_t
  log_size () noexcept
  {
    return g_log_size;
  }

  void
  serve_for (std::uint32_t ms) noexcept
  {
    const std::uint32_t start = platform::millis ();
    do
      {
        net::poll ();
        sysclock.sleep_for (2);
      }
    while (platform::millis () - start < ms);
  }

  bring_up_result
  bring_up (const char* ssid, const char* passphrase) noexcept
  {
    bring_up_result result{};

    kprintf ("\n[NET] bringing up the CYW43436 ...\n");
    const std::uint32_t t0 = platform::millis ();
    if (!wlan::start ())
      {
        kprintf ("[NET] radio bring-up FAILED: %s (chip %u rev %u, firmware "
                 "%s)\n",
                 wlan::last_error (), wlan::chip_id (), wlan::chip_revision (),
                 wlan::firmware_name ());
        log ("radio: FAILED, %s\n", wlan::last_error ());
        std::snprintf (g_detail, sizeof (g_detail), "radio: %s (chip %u rev %u)",
                       wlan::last_error (), wlan::chip_id (),
                       wlan::chip_revision ());
        return result;
      }
    result.radio = true;
    const auto& mac = wlan::address ();
    kprintf ("[NET] chip %u rev %u, firmware %s, up in %u ms\n",
             wlan::chip_id (), wlan::chip_revision (), wlan::firmware_name (),
             static_cast<unsigned> (platform::millis () - t0));
    kprintf ("[NET] %s\n", wlan::firmware_version ());
    kprintf ("[NET] MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    log ("radio: chip %u rev %u, firmware %s\n", wlan::chip_id (),
         wlan::chip_revision (), wlan::firmware_name ());
    log ("firmware: %s\n", wlan::firmware_version ());
    log ("mac: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2],
         mac[3], mac[4], mac[5]);

    // The radio has the Arasan controller now and the card slot is on
    // SDHOST: the SD thread may start.
    saver_start ();
    net::init ();

    const auto networks = wlan::scan ();
    kprintf ("[NET] scan: %u network(s)\n",
             static_cast<unsigned> (networks.size ()));
    log ("scan: %u network(s)\n", static_cast<unsigned> (networks.size ()));
    for (const auto& network : networks)
      {
        const bool target = network.name () == ssid;
        kprintf ("[NET]   %-32s ch %2u %4d dBm %s%s%s%s%s%s\n", network.ssid,
                 static_cast<unsigned> (network.channel),
                 static_cast<int> (network.rssi), network.secured ? "" : "open",
                 network.wpa ? "WPA " : "", network.wpa2 ? "WPA2 " : "",
                 network.tkip ? "TKIP " : "", network.ccmp ? "AES " : "",
                 target ? " <- target" : "");
        log ("  %-32s ch %2u %4d dBm%s\n", network.ssid,
             static_cast<unsigned> (network.channel),
             static_cast<int> (network.rssi), target ? " <- target" : "");
      }
    result.scanned = !networks.empty ();
    if (!result.scanned)
      {
        std::snprintf (g_detail, sizeof (g_detail), "the scan found no network");
        return result;
      }
    if (ssid == nullptr || ssid[0] == '\0')
      {
        kprintf ("[NET] no network configured (WIFI_TEST_SSID)\n");
        std::snprintf (g_detail, sizeof (g_detail),
                       "no network configured (WIFI_TEST_SSID); the scan saw %u",
                       static_cast<unsigned> (networks.size ()));
        return result;
      }

    for (unsigned attempt = 1; attempt <= 2 && !result.joined; ++attempt)
      {
        kprintf ("[NET] join \"%s\", attempt %u ...\n", ssid, attempt);
        result.joined = wlan::join (ssid, passphrase);
        if (!result.joined)
          {
            kprintf ("[NET] %s\n", wlan::last_error ());
            serve_for (1000);
          }
      }
    if (!result.joined)
      {
        log ("join: FAILED, %s\n", wlan::last_error ());
        std::snprintf (g_detail, sizeof (g_detail), "%s", wlan::last_error ());
        return result;
      }
    log ("join: \"%s\" ok\n", ssid);

    if (!net::wait_for_address (20000))
      {
        kprintf ("[NET] no DHCP lease within 20 s\n");
        log ("address: none (no DHCP lease)\n");
        std::snprintf (g_detail, sizeof (g_detail),
                       "joined, but no DHCP lease (rx %u data frames, tx %u)",
                       static_cast<unsigned> (wlan::stats ().rx_data),
                       static_cast<unsigned> (wlan::stats ().tx_data));
        return result;
      }
    result.address = true;
    const std::uint32_t ip = net::address ();
    const std::uint32_t gw = net::gateway ();
    net::print_status ();
    log ("address: %u.%u.%u.%u (DHCP), gateway %u.%u.%u.%u\n",
         static_cast<unsigned> (ip >> 24),
         static_cast<unsigned> ((ip >> 16) & 0xff),
         static_cast<unsigned> ((ip >> 8) & 0xff),
         static_cast<unsigned> (ip & 0xff), static_cast<unsigned> (gw >> 24),
         static_cast<unsigned> ((gw >> 16) & 0xff),
         static_cast<unsigned> ((gw >> 8) & 0xff),
         static_cast<unsigned> (gw & 0xff));
    return result;
  }

  const char*
  failure_detail () noexcept
  {
    return g_detail;
  }

  void
  saver_start () noexcept
  {
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = g_sd_stack;
    attr.th_stack_size_bytes = sizeof (g_sd_stack);
    attr.th_priority = thread::priority::normal;
    static thread sd_thread{ "sd", sd_func, nullptr, attr };
  }

  bool
  saver_ready () noexcept
  {
    return g_sd_ready;
  }

  bool
  saver_failed () noexcept
  {
    return g_sd_failed;
  }

  bool
  saver_request (const char* leaf, const void* data,
                 std::uint32_t size) noexcept
  {
    if (g_sd_busy)
      {
        return false;
      }
    g_req_leaf = leaf;
    g_req_data = data;
    g_req_size = size;
    g_sd_ok = false;
    g_sd_busy = true;
    g_sd_work.post ();
    return true;
  }

  bool
  saver_busy () noexcept
  {
    return g_sd_busy;
  }

  bool
  saver_ok () noexcept
  {
    return g_sd_ok;
  }

  bool
  save_and_wait (const char* leaf, const void* data, std::uint32_t size,
                 std::uint32_t timeout_ms) noexcept
  {
    const std::uint32_t start = platform::millis ();
    while (!saver_ready ())
      {
        if (platform::millis () - start >= timeout_ms)
          {
            return false;
          }
        net::poll ();
        sysclock.sleep_for (2);
      }
    if (!saver_request (leaf, data, size))
      {
        return false;
      }
    while (saver_busy ())
      {
        if (platform::millis () - start >= timeout_ms)
          {
            kprintf ("[SD] tests/%s: no completion within %u ms\n", leaf,
                     static_cast<unsigned> (timeout_ms));
            return false;
          }
        net::poll ();
        sysclock.sleep_for (2);
      }
    return saver_ok ();
  }
} // namespace rpiz2w::wifi_app
