// net.hpp - IPv4 networking over the Wi-Fi link (lwIP, raw API, no OS).
//
//   netif "wl0"   Ethernet frames <-> wlan::send_ethernet / receive handler
//   DHCP client   started when the link comes up
//   ping          ICMP echo to an address, a host name, or "gw"
//   TCP echo      RFC 862 server on port 7; a data handler can observe the
//                 stream, which is how a test saves what it receives
//
// One thread owns the stack: only that thread may call anything here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rpiz2w::net
{
  /// Initialise lwIP and hook the interface to the wlan driver. Idempotent.
  void init () noexcept;

  /// Service the radio and run the lwIP timers (DHCP, ARP, TCP).
  void poll () noexcept;

  [[nodiscard]] bool link_up () noexcept;
  [[nodiscard]] bool has_address () noexcept;

  /// True when the address came from DHCP rather than set_static_address().
  [[nodiscard]] bool leased () noexcept;

  /// The interface address as host-order bytes a.b.c.d; 0 when none.
  [[nodiscard]] std::uint32_t address () noexcept;
  [[nodiscard]] std::uint32_t gateway () noexcept;

  /// Print link, address, mask, gateway, DNS and the echo-server counters.
  void print_status () noexcept;

  /// Stop DHCP and use a static address, e.g. ("192.168.1.74/24",
  /// "192.168.1.1").
  bool set_static_address (std::string_view address,
                           std::string_view gateway) noexcept;

  /// Wait for an address; false after `timeout_ms`.
  bool wait_for_address (std::uint32_t timeout_ms) noexcept;

  /// Send `count` ICMP echo requests one second apart and print each reply.
  /// `target` is a dotted address, a host name (resolved with DNS) or "gw".
  /// Returns the number of replies.
  unsigned ping (std::string_view target, unsigned count,
                 unsigned payload_size = 56) noexcept;

  inline constexpr std::uint16_t echo_port = 7;

  /// Called on the owning thread for every piece of data the echo server
  /// has taken from a connection, in order.
  using echo_data_handler = void (*) (const std::uint8_t* data,
                                      std::size_t size) noexcept;
  /// Called when a connection ends, with the bytes it carried.
  using echo_close_handler = void (*) (std::uint64_t bytes) noexcept;
  void set_echo_handlers (echo_data_handler on_data,
                          echo_close_handler on_close) noexcept;

  [[nodiscard]] std::uint32_t echo_connections_total () noexcept;
  [[nodiscard]] std::uint64_t echo_bytes_total () noexcept;
} // namespace rpiz2w::net
