/*
 * wifi-tcp-server — a TCP server on the Raspberry Pi Zero 2 W Wi-Fi that
 * saves what it receives to the SD card: a file transfer from the host.
 *
 * The Zero 2 W counterpart of the Pi 4 B eth-tcp-server test: the radio
 * (CYW43436 on the Arasan controller), lwIP, and the SD card on the other
 * controller (SDHOST), each owned by its own thread. The server is the RFC
 * 862 echo service on port 7: every byte a client sends comes back, and is
 * also kept in RAM. When the client closes the connection the received
 * stream is written to tests/WIFIRX.BIN on the SD card (by the SD thread,
 * while the net thread keeps the radio serviced) and read back.
 *
 *   host                                board
 *   ----------------------------------  ---------------------------------
 *                                       radio, scan, join, DHCP
 *                                       prints "TCP server ready at IP:7"
 *   wifi-tcp-host.py connects, sends    echoes, keeps the bytes
 *   a file or N pattern bytes, checks
 *   the echo, closes
 *                                       saves tests/WIFIRX.BIN, verifies it,
 *                                       prints size + CRC-32, RESULT
 *
 *   ./wifi-tcp-host.py 192.168.1.65 --size 262144
 *   ./wifi-tcp-host.py 192.168.1.65 --file some-file.bin
 *
 * Verdict: PASS when a client connected and sent data (a connection that
 * carries nothing is ignored), closed, and the stream was saved to the SD
 * card and read back unchanged. Without a client within WIFI_TCP_WAIT_S
 * seconds (default 60) the test FAILs.
 *
 * Configure-time options: WIFI_TEST_SSID, WIFI_TEST_PASS (see wifi-net),
 * WIFI_TCP_WAIT_S.
 */

#include <cmsis-plus/rtos/os.h>

#include <hw_result.hpp>
#include <uart.hpp>

#include "wifi_app.hpp"
#include "wlan.hpp"
#include "net.hpp"
#include "platform.hpp"

#include <cstdint>
#include <cstring>

#ifndef WIFI_TEST_SSID
#define WIFI_TEST_SSID ""
#endif
#ifndef WIFI_TEST_PASS
#define WIFI_TEST_PASS ""
#endif
#ifndef WIFI_TCP_WAIT_S
#define WIFI_TCP_WAIT_S 60
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
  const char* volatile g_reason = "see the log above";

  // What the client sent, in arrival order. Only the net thread writes it;
  // the SD thread reads it after the connection has closed.
  constexpr std::uint32_t capture_limit = 8u * 1024u * 1024u;
  std::uint8_t g_capture[capture_limit];
  std::uint32_t g_captured = 0;
  std::uint64_t g_received = 0;
  std::uint32_t g_crc = 0xffffffffu;
  volatile bool g_closed = false;

  // CRC-32 (IEEE 802.3, the zlib one), bitwise: the host compares with
  // zlib.crc32().
  std::uint32_t
  crc32_update (std::uint32_t crc, const std::uint8_t* data,
                std::size_t size) noexcept
  {
    for (std::size_t i = 0; i < size; ++i)
      {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
          {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
          }
      }
    return crc;
  }

  void
  on_data (const std::uint8_t* data, std::size_t size) noexcept
  {
    g_crc = crc32_update (g_crc, data, size);
    g_received += size;
    const std::uint32_t room = capture_limit - g_captured;
    const std::uint32_t take
        = size < room ? static_cast<std::uint32_t> (size) : room;
    std::memcpy (g_capture + g_captured, data, take);
    g_captured += take;
  }

  void
  on_close (std::uint64_t bytes) noexcept
  {
    // A connection that carried nothing (a port scan, a probe) is not the
    // client: keep waiting for one that sends data.
    if (bytes > 0)
      {
        g_closed = true;
      }
  }

  void*
  net_func (void*)
  {
    const auto up = app::bring_up (WIFI_TEST_SSID, WIFI_TEST_PASS);

    bool client = false;
    bool saved = false;
    if (up.address)
      {
        net::set_echo_handlers (on_data, on_close);
        const std::uint32_t ip = net::address ();
        // The host script waits for exactly this line.
        kprintf ("[TEST] TCP server ready at %u.%u.%u.%u:%u, waiting %u s "
                 "for a client\n",
                 static_cast<unsigned> (ip >> 24),
                 static_cast<unsigned> ((ip >> 16) & 0xff),
                 static_cast<unsigned> ((ip >> 8) & 0xff),
                 static_cast<unsigned> (ip & 0xff),
                 static_cast<unsigned> (net::echo_port),
                 static_cast<unsigned> (WIFI_TCP_WAIT_S));

        // Wait for the first connection, then for it to finish. A transfer
        // in progress is never cut: the limit applies while nothing arrives.
        std::uint32_t idle_since = platform::millis ();
        std::uint64_t seen = 0;
        while (!g_closed)
          {
            net::poll ();
            if (g_received != seen)
              {
                client = true;
              }
            if (g_received != seen)
              {
                seen = g_received;
                idle_since = platform::millis ();
              }
            if (platform::millis () - idle_since
                >= static_cast<std::uint32_t> (WIFI_TCP_WAIT_S) * 1000u)
              {
                break;
              }
            sysclock.sleep_for (2);
          }
        // Let the final ACKs and the FIN handshake go out.
        app::serve_for (200);

        const std::uint32_t crc = g_crc ^ 0xffffffffu;
        if (g_closed && g_received > 0)
          {
            kprintf ("[TEST] received %lu bytes, CRC-32 %08x%s\n",
                     static_cast<unsigned long> (g_received),
                     static_cast<unsigned> (crc),
                     g_received > g_captured ? " (stream longer than the "
                                               "capture buffer)"
                                             : "");
            app::log ("tcp: %lu bytes received and echoed, CRC-32 %08x\n",
                      static_cast<unsigned long> (g_received),
                      static_cast<unsigned> (crc));
            saved = app::save_and_wait ("WIFIRX.BIN", g_capture, g_captured,
                                        120000);
            app::log ("sd: tests/WIFIRX.BIN %u bytes %s\n",
                      static_cast<unsigned> (g_captured),
                      saved ? "saved and verified" : "FAILED");
          }
        else if (!client)
          {
            kprintf ("[TEST] no client connected within %u s\n",
                     static_cast<unsigned> (WIFI_TCP_WAIT_S));
            app::log ("tcp: no client\n");
          }
        else
          {
            kprintf ("[TEST] the client sent data but never closed the "
                     "connection\n");
            app::log ("tcp: connection without close\n");
          }
      }

    const auto& stat = wlan::stats ();
    kprintf ("[NET] rx %u data frames (%u errors), tx %u (%u errors)\n",
             static_cast<unsigned> (stat.rx_data),
             static_cast<unsigned> (stat.rx_errors),
             static_cast<unsigned> (stat.tx_data),
             static_cast<unsigned> (stat.tx_errors));
    app::log ("frames: rx %u data (%u errors), tx %u (%u errors)\n",
              static_cast<unsigned> (stat.rx_data),
              static_cast<unsigned> (stat.rx_errors),
              static_cast<unsigned> (stat.tx_data),
              static_cast<unsigned> (stat.tx_errors));

    const bool pass = up.address && g_closed && g_received > 0 && saved;
    app::log ("result: %s\n", pass ? "PASS" : "FAIL");
    // The log too; its result does not change the verdict of the transfer.
    (void)app::save_and_wait ("WIFITCP.LOG", app::log_text (),
                              app::log_size ());

    if (!up.address)
      {
        // The bring-up says what stopped it, with the PHY or frame counts.
        g_reason = app::failure_detail ();
      }
    else if (!client)
      {
        g_reason = "no TCP client connected";
      }
    else if (!g_closed || g_received == 0)
      {
        g_reason = "the client did not close the connection";
      }
    else if (!saved)
      {
        g_reason = "the stream was not saved to the SD card";
      }
    g_pass = pass;
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
  // The emulated image: QEMU's machine is not asked for a network here, so
  // there is nothing to test. SKIP counts as a pass for the runner.
  kprintf ("\nRESULT: SKIP (%s needs the real radio and an access point)\n",
           "wifi-tcp-server");
  hw_result::ok ();
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
#endif
  kprintf ("\n=== wifi-tcp-server: TCP echo on port 7, stream saved to the SD "
           "card (" PORT_BANNER_LONG ") ===\n");
  app::log ("wifi-tcp-server on " PORT_BANNER_LONG "\n");

  app::saver_start ();

  thread::attributes attr = thread::initializer;
  attr.th_stack_address = net_stack;
  attr.th_stack_size_bytes = sizeof (net_stack);
  attr.th_priority = thread::priority::above_normal;
  static thread net_thread{ "net", net_func, nullptr, attr };

  // Bring-up, the wait for a client, the transfer and two SD writes.
  constexpr std::uint32_t timeout_ms
      = (60u + static_cast<std::uint32_t> (WIFI_TCP_WAIT_S) + 240u) * 1000u;
  const std::uint32_t start = platform::millis ();
  while (g_done.try_wait () != result::ok)
    {
      if (platform::millis () - start >= timeout_ms)
        {
          platform::dump_log ();
          kprintf ("\nRESULT: FAIL (timeout)\n");
          hw_result::fail ();
          for (;;)
            {
              sysclock.sleep_for (1000);
            }
        }
      sysclock.sleep_for (100);
    }

  // The whole console, for the runner: the UART is the user's.
  platform::dump_log ();
  if (g_pass)
    {
      kprintf ("\nRESULT: PASS (client served, stream on the SD card)\n");
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
