// wifi_app.cpp - the shared bring-up script and the SD card thread.
#include "wifi_app.hpp"

#include "wlan.hpp"
#include "net.hpp"
#include "platform.hpp"

#include <cmsis-plus/rtos/os.h>

#include <fatfs_hw.hpp>
#include <sd.hpp>

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

    bool
    store_and_verify (const char* leaf, const void* data, std::uint32_t size)
    {
      (void)fatfshw::remove_file (leaf);
      if (!fatfshw::store_file (leaf, data, size))
        {
          kprintf ("[SD] write tests/%s failed: %s\n", leaf,
                   fatfshw::last_error ());
          return false;
        }
      std::uint32_t stored = 0;
      if (!fatfshw::file_size (leaf, stored) || stored != size)
        {
          kprintf ("[SD] tests/%s is %u bytes, expected %u\n", leaf,
                   static_cast<unsigned> (stored),
                   static_cast<unsigned> (size));
          return false;
        }
      const auto* expected = static_cast<const std::uint8_t*> (data);
      for (std::uint32_t at = 0; at < size;)
        {
          const std::uint32_t piece
              = size - at < sizeof (g_verify) ? size - at : sizeof (g_verify);
          if (!fatfshw::load_file_at (leaf, at, g_verify, piece)
              || std::memcmp (g_verify, expected + at, piece) != 0)
            {
              kprintf ("[SD] tests/%s differs at offset %u\n", leaf,
                       static_cast<unsigned> (at));
              return false;
            }
          at += piece;
        }
      kprintf ("[SD] saved tests/%s, %u bytes, read back and verified\n",
               leaf, static_cast<unsigned> (size));
      return true;
    }

    void*
    sd_func (void*)
    {
      // The card is on SDHOST (SD_BACKEND_SDHOST): the Arasan is the radio's.
      static sd::SdCard card;
      if (!card.init ())
        {
          kprintf ("[SD] card init failed: %s\n", card.last_error ());
          g_sd_failed = true;
        }
      else
        {
          fatfshw::bind_card (card);
          if (!fatfshw::mount_volume () || !fatfshw::ensure_tests_dir ())
            {
              kprintf ("[SD] mount failed: %s\n", fatfshw::last_error ());
              g_sd_failed = true;
            }
          else
            {
              kprintf ("[SD] card ready: %u MiB, FAT volume mounted, files "
                       "go to tests/\n",
                       static_cast<unsigned> (card.capacity_bytes () >> 20));
            }
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
