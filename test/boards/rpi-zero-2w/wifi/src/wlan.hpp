// wlan.hpp - the Raspberry Pi Zero 2 W Wi-Fi: Broadcom/Cypress CYW43436
// (a "FullMAC" 802.11n chip: scanning, association and the WPA2 handshake
// run in the chip's own firmware) on SDIO.
//
// A polled driver, no interrupts: one thread owns it and calls poll().
//
//   start()           power the chip, download firmware + NVRAM (+ CLM),
//                     start it, configure it, read the MAC address
//   scan()            active scan; the networks found
//   join()            associate with an access point (WPA2-PSK or open)
//   poll()            hand received frames to the receive handler, follow
//                     link events
//   send_ethernet()   transmit one Ethernet frame
//
// The interface is the one the Mango Pi MQ-Pro radio and the Pi 4 B
// Ethernet driver offer, so the lwIP glue (net.cpp) is the same.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rpiz2w::wlan
{
  using mac_address = std::array<std::uint8_t, 6>;
  using receive_handler = void (*) (std::span<const std::uint8_t> frame) noexcept;
  using link_handler = void (*) (bool up) noexcept;

  struct network
  {
    char ssid[33]{};
    mac_address bssid{};
    std::int16_t rssi{ 0 };
    std::uint8_t channel{ 0 };
    bool secured{ false };
    bool wpa{ false };   ///< advertises WPA (the vendor element)
    bool wpa2{ false };  ///< advertises WPA2 (the RSN element)
    bool tkip{ false };  ///< TKIP among the pairwise ciphers
    bool ccmp{ false };  ///< AES-CCMP among the pairwise ciphers

    [[nodiscard]] std::string_view
    name () const noexcept
    {
      return ssid;
    }
  };

  struct statistics
  {
    std::uint32_t rx_frames{ 0 };  ///< SDPCM frames of any kind
    std::uint32_t rx_data{ 0 };    ///< Ethernet frames delivered
    std::uint32_t rx_events{ 0 };
    std::uint32_t rx_errors{ 0 };
    std::uint32_t tx_data{ 0 };
    std::uint32_t tx_errors{ 0 };
    std::uint32_t ioctls{ 0 };
    std::uint32_t ioctl_errors{ 0 };
  };

  /// Bring the radio up. False when the chip does not answer, is not a
  /// supported one, or its firmware does not start; see last_error().
  [[nodiscard]] bool start () noexcept;

  /// Scan for about `ms` milliseconds; the strongest entry per network name.
  [[nodiscard]] std::span<const network> scan (std::uint32_t ms = 8000) noexcept;

  /// Join a network. An empty passphrase means an open network; otherwise
  /// WPA2-PSK or WPA-PSK, with AES or TKIP, as the network advertised in the
  /// last scan (WPA2 when it was not seen). Blocks up to `timeout_ms`,
  /// servicing the radio.
  [[nodiscard]] bool join (std::string_view ssid, std::string_view passphrase,
                           std::uint32_t timeout_ms = 15000) noexcept;

  /// Service the receive path and the events. Call often (every 1-2 ms).
  void poll () noexcept;

  /// Send one Ethernet frame (destination, source, type, payload).
  [[nodiscard]] bool send_ethernet (std::span<const std::uint8_t> frame) noexcept;

  void set_receive_handler (receive_handler handler) noexcept;
  void set_link_handler (link_handler handler) noexcept;

  [[nodiscard]] const mac_address& address () noexcept;
  [[nodiscard]] bool linked () noexcept;
  /// Chip identifier (43430 for the CYW43436) and silicon revision.
  [[nodiscard]] unsigned chip_id () noexcept;
  [[nodiscard]] unsigned chip_revision () noexcept;
  /// The firmware image chosen for this chip ("43436", "43436s").
  [[nodiscard]] const char* firmware_name () noexcept;
  /// The firmware's own version line (iovar "ver"), after start().
  [[nodiscard]] const char* firmware_version () noexcept;
  [[nodiscard]] const statistics& stats () noexcept;
  /// Where a failed start() or join() stopped.
  [[nodiscard]] const char* last_error () noexcept;
} // namespace rpiz2w::wlan
