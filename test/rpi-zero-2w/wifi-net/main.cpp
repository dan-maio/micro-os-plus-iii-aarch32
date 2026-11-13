/*
 * wifi-net — the Raspberry Pi Zero 2 W Wi-Fi (CYW43436 on SDIO) and lwIP
 * under the µOS++ scheduler: firmware download, scan, join, DHCP, ping.
 *
 * The Zero 2 W counterpart of the Mango Pi MQ-Pro wifi-net test and of the
 * Pi 4 B eth-net test, with the same shape: one thread owns the radio and
 * lwIP and polls every 2 ms; os_main starts it and waits for the verdict.
 *
 * A second thread owns the SD card, on the SDHOST controller (the radio has
 * the Arasan one), and saves the run's log while the radio stays serviced.
 *
 * Verdict: PASS when the chip answers and its firmware starts, a scan sees at
 * least one network, the station joins the configured network, DHCP hands
 * out an address, the gateway answers an ICMP echo, and the run's log is
 * written to tests/WIFINET.LOG on the SD card and read back unchanged.
 *
 * Configure-time options (cmake -D NAME=value); nothing is stored in the
 * repository:
 *   WIFI_TEST_SSID      the network to join
 *   WIFI_TEST_PASS      its WPA2 passphrase (empty: an open network)
 *   WIFI_TEST_GATEWAY   gateway to ping; default: the one DHCP gave
 *   WIFI_COUNTRY        two-letter regulatory domain, default "XX"
 */

#include <cmsis-plus/rtos/os.h>

#include <hw_result.hpp>
#include <uart.hpp>

#include "net.hpp"
#include "platform.hpp"
#include "wifi_app.hpp"
#include "wlan.hpp"

#include <cstdint>
#include <cstdio>

#ifndef WIFI_TEST_SSID
#define WIFI_TEST_SSID ""
#endif
#ifndef WIFI_TEST_PASS
#define WIFI_TEST_PASS ""
#endif
#ifndef WIFI_TEST_GATEWAY
#define WIFI_TEST_GATEWAY ""
#endif

using namespace os::rtos;
namespace app = rpiz2w::wifi_app;
namespace net = rpiz2w::net;
namespace wlan = rpiz2w::wlan;
namespace platform = rpiz2w::platform;

namespace
{
  alignas (16) std::uint8_t net_stack[32 * 1024];

  semaphore_binary g_done{ "net-done", 0 };
  volatile bool g_pass = false;
  char g_reason[160] = "see the log above";

  void*
  net_func (void*)
  {
    const auto up = app::bring_up (WIFI_TEST_SSID, WIFI_TEST_PASS);

    // --- ping -------------------------------------------------------------
    unsigned replies = 0;
    if (up.address)
      {
        kprintf ("[NET] ping the gateway ...\n");
        replies = net::ping (
            WIFI_TEST_GATEWAY[0] != '\0' ? WIFI_TEST_GATEWAY : "gw", 4);
        app::log ("ping gateway: %u of 4 replies\n", replies);
      }

    // --- counters ---------------------------------------------------------
    const auto& stat = wlan::stats ();
    kprintf ("[NET] rx %u frames (%u data, %u events, %u errors), tx %u data "
             "(%u errors), %u ioctls (%u errors)\n",
             static_cast<unsigned> (stat.rx_frames),
             static_cast<unsigned> (stat.rx_data),
             static_cast<unsigned> (stat.rx_events),
             static_cast<unsigned> (stat.rx_errors),
             static_cast<unsigned> (stat.tx_data),
             static_cast<unsigned> (stat.tx_errors),
             static_cast<unsigned> (stat.ioctls),
             static_cast<unsigned> (stat.ioctl_errors));
    app::log ("frames: rx %u data (%u errors), tx %u (%u errors)\n",
              static_cast<unsigned> (stat.rx_data),
              static_cast<unsigned> (stat.rx_errors),
              static_cast<unsigned> (stat.tx_data),
              static_cast<unsigned> (stat.tx_errors));

    const bool network = up.address && replies > 0;
    app::log ("network: %s\n", network ? "PASS" : "FAIL");

    // --- save the log -----------------------------------------------------
    // The radio stays serviced while the card is written. Only once the
    // radio is up: before that the card slot is not on its controller yet.
    const bool saved = up.radio
                       && app::save_and_wait ("WIFINET.LOG", app::log_text (),
                                              app::log_size ());

    if (!up.address)
      {
        std::snprintf (g_reason, sizeof (g_reason), "%s", app::failure_detail ());
      }
    else if (replies == 0)
      {
        std::snprintf (g_reason, sizeof (g_reason), "the gateway did not answer");
      }
    else if (!saved)
      {
        std::snprintf (g_reason, sizeof (g_reason),
                       "the log was not saved to the SD card");
      }
    else
      {
        std::snprintf (g_reason, sizeof (g_reason),
                       "joined, leased, gateway ping, log on the SD card");
      }
    g_pass = network && saved;
    g_done.post ();

    for (;;)
      {
        net::poll ();
        sysclock.sleep_for (2);
      }
    return nullptr;
  }
} // namespace

int
os_main (int, char*[])
{
#if !defined(HW_BUILD)
  // The emulated machine has no CYW43436. SKIP counts as a pass.
  kprintf ("\nRESULT: SKIP (wifi-net needs the real radio)\n");
  hw_result::ok ();
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
#endif
  kprintf ("\n=== wifi-net: CYW43436 + lwIP on micro-os-plus (" PORT_BANNER_LONG
           ") ===\n"
           "[TEST] ssid \"%s\"\n"
           "[TEST] threads: net (radio + lwIP, above normal, 32 KiB, 2 ms "
           "poll)\n"
           "[TEST]          sd  (SD card on SDHOST + FatFs, normal, 64 KiB, "
           "on request)\n",
           WIFI_TEST_SSID);
  app::log ("wifi-net on " PORT_BANNER_LONG "\n");

  thread::attributes attr = thread::initializer;
  attr.th_stack_address = net_stack;
  attr.th_stack_size_bytes = sizeof (net_stack);
  attr.th_priority = thread::priority::above_normal;
  static thread net_thread{ "net", net_func, nullptr, attr };

  constexpr std::uint32_t timeout_ms = 150 * 1000;
  const std::uint32_t start = platform::millis ();
  bool timed_out = false;
  while (g_done.try_wait () != result::ok)
    {
      if (platform::millis () - start >= timeout_ms)
        {
          timed_out = true;
          break;
        }
      sysclock.sleep_for (100);
    }

  // The whole console, for the runner: the UART is the user's.
  platform::dump_log ();
  if (timed_out)
    {
      kprintf ("\nRESULT: FAIL (timeout)\n");
      hw_result::fail ();
    }
  else if (g_pass)
    {
      kprintf ("\nRESULT: PASS (%s)\n", g_reason);
      hw_result::ok ();
    }
  else
    {
      kprintf ("\nRESULT: FAIL (%s)\n", g_reason);
      hw_result::fail ();
    }
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
  return 0;
}

#include "wifi_startup.hpp"
