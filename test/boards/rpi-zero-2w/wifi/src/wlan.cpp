// wlan.cpp - CYW43436 (chip id 43430) FullMAC Wi-Fi on SDIO, polled.
//
// What the chip is, in the order the code meets it:
//
//   SDIO function 1   the "backplane": a 32 KiB window onto the chip's
//                     internal bus, moved with three address registers.
//                     Through it the driver halts the chip's ARM core,
//                     writes the firmware into its RAM and starts it.
//   SDIO function 2   the data path, alive once the firmware runs. Every
//                     transfer is an SDPCM frame on one of three channels:
//                       0 control  request/response ("ioctl", CDC header)
//                       1 event    asynchronous events from the firmware
//                       2 data     Ethernet frames (BDC header)
//
// The register values and the bring-up order follow Linux brcmfmac and the
// Infineon WHD documentation; no code is taken from either.
#include "wlan.hpp"

#include "platform.hpp"
#include "sdio.hpp"

#include <cstdio>
#include <cstring>

extern "C"
{
  extern const std::uint8_t cyw43_fw_43436[];
  extern const std::uint8_t cyw43_fw_43436_end[];
  extern const std::uint8_t cyw43_nvram_43436[];
  extern const std::uint8_t cyw43_nvram_43436_end[];
  extern const std::uint8_t cyw43_clm_43436[];
  extern const std::uint8_t cyw43_clm_43436_end[];
  extern const std::uint8_t cyw43_fw_43436s[];
  extern const std::uint8_t cyw43_fw_43436s_end[];
  extern const std::uint8_t cyw43_nvram_43436s[];
  extern const std::uint8_t cyw43_nvram_43436s_end[];
}

#ifndef WIFI_COUNTRY
#define WIFI_COUNTRY "XX" // the firmware's world-wide domain
#endif

namespace rpiz2w::wlan
{
  namespace
  {
    // ------------------------------------------------- function 1 registers
    constexpr std::uint32_t F1_SBADDRLOW = 0x1000a;
    constexpr std::uint32_t F1_SBADDRMID = 0x1000b;
    constexpr std::uint32_t F1_SBADDRHIGH = 0x1000c;
    constexpr std::uint32_t F1_CHIPCLKCSR = 0x1000e;
    constexpr std::uint32_t F1_SDIOPULLUP = 0x1000f;
    constexpr std::uint32_t F1_WATERMARK = 0x10008;
    constexpr std::uint8_t CSR_ALP_AVAIL_REQ = 0x08;
    constexpr std::uint8_t CSR_ALP_AVAIL = 0x40;
    constexpr std::uint8_t CSR_HT_AVAIL = 0x80;
    constexpr std::uint32_t window_mask = 0x7fff;
    constexpr std::uint32_t access_4b = 0x8000;

    // ------------------------------------------------------ backplane cores
    constexpr std::uint32_t CHIPCOMMON = 0x18000000;
    constexpr std::uint32_t SDIO_CORE = 0x18002000;
    constexpr std::uint32_t ARM_CORE = 0x18003000;
    constexpr std::uint32_t SOCRAM_CORE = 0x18004000;
    constexpr std::uint32_t WRAPPER = 0x00100000;
    constexpr std::uint32_t AI_IOCTRL = 0x408;
    constexpr std::uint32_t AI_RESETCTRL = 0x800;
    constexpr std::uint32_t IOCTRL_CLOCK_EN = 0x01;
    constexpr std::uint32_t IOCTRL_FGC = 0x02;
    constexpr std::uint32_t RESETCTRL_RESET = 0x01;
    constexpr std::uint32_t SOCRAM_BANKX_INDEX = SOCRAM_CORE + 0x10;
    constexpr std::uint32_t SOCRAM_BANKX_PDA = SOCRAM_CORE + 0x44;
    constexpr std::uint32_t SDIO_INT_HOST_MASK = SDIO_CORE + 0x24;
    constexpr std::uint32_t ram_base = 0x00000000;
    constexpr std::uint32_t ram_size = 512u * 1024u;

    // ---------------------------------------------------------------- ioctl
    constexpr std::uint32_t WLC_UP = 2;
    constexpr std::uint32_t WLC_SET_INFRA = 20;
    constexpr std::uint32_t WLC_SET_AUTH = 22;
    constexpr std::uint32_t WLC_SET_SSID = 26;
    constexpr std::uint32_t WLC_DISASSOC = 52;
    constexpr std::uint32_t WLC_SET_PM = 86;
    constexpr std::uint32_t WLC_SET_WSEC = 134;
    constexpr std::uint32_t WLC_SET_WPA_AUTH = 165;
    constexpr std::uint32_t WLC_GET_VAR = 262;
    constexpr std::uint32_t WLC_SET_VAR = 263;
    constexpr std::uint32_t WLC_SET_WSEC_PMK = 268;

    // --------------------------------------------------------------- events
    constexpr std::uint32_t EV_SET_SSID = 0;
    constexpr std::uint32_t EV_DEAUTH = 5;
    constexpr std::uint32_t EV_DEAUTH_IND = 6;
    constexpr std::uint32_t EV_DISASSOC = 11;
    constexpr std::uint32_t EV_DISASSOC_IND = 12;
    constexpr std::uint32_t EV_LINK = 16;
    constexpr std::uint32_t EV_PSK_SUP = 46;
    constexpr std::uint32_t EV_ESCAN_RESULT = 69;

    constexpr std::size_t sdpcm_header = 12;
    constexpr std::size_t cdc_header = 16;
    constexpr std::size_t frame_max = 2048;

    // ---------------------------------------------------------------- state
    alignas (8) std::uint8_t g_tx[frame_max];
    alignas (8) std::uint8_t g_rx[frame_max];
    alignas (8) std::uint8_t g_reply[frame_max];
    alignas (8) std::uint8_t g_chunk[frame_max];

    const char* g_error = "none";
    char g_error_text[96];
    bool g_started = false;
    bool g_linked = false;
    unsigned g_chip = 0;
    unsigned g_chip_rev = 0;
    const char* g_fw_name = "none";
    char g_fw_version[96] = "";
    mac_address g_address{};
    receive_handler g_on_frame = nullptr;
    link_handler g_on_link = nullptr;
    statistics g_stats{};

    std::uint32_t g_window = 0xffffffffu;
    std::uint8_t g_tx_seq = 0;
    std::uint8_t g_tx_credit = 1; // one frame may go before the first header
    std::uint16_t g_ioctl_id = 0;
    bool g_reply_seen = false;
    std::uint16_t g_reply_id = 0;
    std::uint32_t g_reply_status = 0;
    std::size_t g_reply_size = 0;

    // Join progress, set from events.
    bool g_ev_ssid_done = false;
    std::uint32_t g_ev_ssid_status = 0;
    bool g_ev_link_up = false;
    bool g_ev_keyed = false;
    bool g_ev_auth_failed = false;
    std::uint32_t g_ev_last = 0xffffffffu;
    std::uint32_t g_ev_last_status = 0;
    std::uint32_t g_ev_last_reason = 0;

    // Received Ethernet frames wait here until poll() hands them over. A
    // frame can arrive while the driver is waiting inside a transmit or a
    // control request, which may itself have been called from the network
    // stack's receive path; delivering there would re-enter a stack that is
    // not reentrant. So nothing is delivered from inside the driver.
    constexpr std::size_t pending_max = 32;
    constexpr std::size_t pending_frame = 1600;
    alignas (8) std::uint8_t g_pending[pending_max][pending_frame];
    std::uint16_t g_pending_size[pending_max];
    std::size_t g_pending_head = 0;
    std::size_t g_pending_count = 0;
    std::uint32_t g_pending_dropped = 0;

    constexpr std::size_t scan_max = 24;
    network g_networks[scan_max];
    std::size_t g_network_count = 0;
    bool g_scan_done = false;

    bool
    fail (const char* what) noexcept
    {
      g_error = what;
      return false;
    }

    inline std::uint32_t
    le32 (const std::uint8_t* p) noexcept
    {
      return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
             | (static_cast<std::uint32_t> (p[2]) << 16)
             | (static_cast<std::uint32_t> (p[3]) << 24);
    }

    inline std::uint16_t
    le16 (const std::uint8_t* p) noexcept
    {
      return static_cast<std::uint16_t> (p[0] | (p[1] << 8));
    }

    inline std::uint32_t
    be32 (const std::uint8_t* p) noexcept
    {
      return (static_cast<std::uint32_t> (p[0]) << 24)
             | (static_cast<std::uint32_t> (p[1]) << 16)
             | (static_cast<std::uint32_t> (p[2]) << 8) | p[3];
    }

    inline void
    put32 (std::uint8_t* p, std::uint32_t v) noexcept
    {
      p[0] = static_cast<std::uint8_t> (v);
      p[1] = static_cast<std::uint8_t> (v >> 8);
      p[2] = static_cast<std::uint8_t> (v >> 16);
      p[3] = static_cast<std::uint8_t> (v >> 24);
    }

    inline void
    put16 (std::uint8_t* p, std::uint16_t v) noexcept
    {
      p[0] = static_cast<std::uint8_t> (v);
      p[1] = static_cast<std::uint8_t> (v >> 8);
    }

    // ------------------------------------------------------------ backplane
    bool
    set_window (std::uint32_t address) noexcept
    {
      const std::uint32_t base = address & ~window_mask;
      if (base == g_window)
        {
          return true;
        }
      if (!sdio::write8 (1, F1_SBADDRLOW, static_cast<std::uint8_t> ((base >> 8) & 0x80))
          || !sdio::write8 (1, F1_SBADDRMID, static_cast<std::uint8_t> (base >> 16))
          || !sdio::write8 (1, F1_SBADDRHIGH, static_cast<std::uint8_t> (base >> 24)))
        {
          g_window = 0xffffffffu;
          return false;
        }
      g_window = base;
      return true;
    }

    bool
    bp_read32 (std::uint32_t address, std::uint32_t& value) noexcept
    {
      alignas (4) std::uint8_t raw[4];
      if (!set_window (address)
          || !sdio::read (1, (address & window_mask) | access_4b, raw, 4))
        {
          return false;
        }
      value = le32 (raw);
      return true;
    }

    bool
    bp_write32 (std::uint32_t address, std::uint32_t value) noexcept
    {
      alignas (4) std::uint8_t raw[4];
      put32 (raw, value);
      return set_window (address)
             && sdio::write (1, (address & window_mask) | access_4b, raw, 4);
    }

    // Write a run of bytes into the chip's memory. Pieces never cross a
    // window; whole blocks go in block mode, the tail in byte mode.
    bool
    bp_write_memory (std::uint32_t address, const std::uint8_t* data,
                     std::size_t size) noexcept
    {
      constexpr std::size_t piece_max = 1024; // 16 blocks
      while (size != 0)
        {
          std::size_t n = size < piece_max ? size : piece_max;
          const std::size_t to_window_end
              = (window_mask + 1u) - (address & window_mask);
          if (n > to_window_end)
            {
              n = to_window_end;
            }
          if (n > sdio::block_size)
            {
              n -= n % sdio::block_size;
            }
          // A copy: the source may be unaligned, and a short tail is padded.
          std::memset (g_chunk, 0, (n + 3u) & ~std::size_t{ 3 });
          std::memcpy (g_chunk, data, n);
          const std::size_t wire
              = n > sdio::block_size ? n : ((n + 3u) & ~std::size_t{ 3 });
          if (!set_window (address)
              || !sdio::write (1, (address & window_mask) | access_4b, g_chunk,
                               wire))
            {
              return false;
            }
          address += static_cast<std::uint32_t> (n);
          data += n;
          size -= n;
        }
      return true;
    }

    bool
    bp_read_memory (std::uint32_t address, std::uint8_t* data,
                    std::size_t size) noexcept
    {
      // Small reads only (verification): 64 bytes at most per piece.
      while (size != 0)
        {
          std::size_t n = size < sdio::block_size ? size : sdio::block_size;
          const std::size_t to_window_end
              = (window_mask + 1u) - (address & window_mask);
          if (n > to_window_end)
            {
              n = to_window_end;
            }
          if (!set_window (address)
              || !sdio::read (1, (address & window_mask) | access_4b, g_chunk,
                              (n + 3u) & ~std::size_t{ 3 }))
            {
              return false;
            }
          std::memcpy (data, g_chunk, n);
          address += static_cast<std::uint32_t> (n);
          data += n;
          size -= n;
        }
      return true;
    }

    // ---------------------------------------------------------------- cores
    bool
    core_disable (std::uint32_t core) noexcept
    {
      const std::uint32_t wrapper = core + WRAPPER;
      std::uint32_t reset = 0;
      if (!bp_read32 (wrapper + AI_RESETCTRL, reset))
        {
          return false;
        }
      if ((reset & RESETCTRL_RESET) != 0)
        {
          return true; // already held in reset
        }
      std::uint32_t dummy = 0;
      if (!bp_write32 (wrapper + AI_IOCTRL, 0) || !bp_read32 (wrapper + AI_IOCTRL, dummy))
        {
          return false;
        }
      platform::delay_us (1000);
      if (!bp_write32 (wrapper + AI_RESETCTRL, RESETCTRL_RESET))
        {
          return false;
        }
      platform::delay_us (1000);
      return true;
    }

    bool
    core_reset (std::uint32_t core) noexcept
    {
      const std::uint32_t wrapper = core + WRAPPER;
      std::uint32_t dummy = 0;
      if (!core_disable (core)
          || !bp_write32 (wrapper + AI_IOCTRL, IOCTRL_FGC | IOCTRL_CLOCK_EN)
          || !bp_read32 (wrapper + AI_IOCTRL, dummy)
          || !bp_write32 (wrapper + AI_RESETCTRL, 0))
        {
          return false;
        }
      platform::delay_us (1000);
      if (!bp_write32 (wrapper + AI_IOCTRL, IOCTRL_CLOCK_EN)
          || !bp_read32 (wrapper + AI_IOCTRL, dummy))
        {
          return false;
        }
      platform::delay_us (1000);
      return true;
    }

    bool
    core_is_up (std::uint32_t core) noexcept
    {
      const std::uint32_t wrapper = core + WRAPPER;
      std::uint32_t io = 0;
      std::uint32_t reset = 0;
      return bp_read32 (wrapper + AI_IOCTRL, io)
             && bp_read32 (wrapper + AI_RESETCTRL, reset)
             && (io & (IOCTRL_FGC | IOCTRL_CLOCK_EN)) == IOCTRL_CLOCK_EN
             && (reset & RESETCTRL_RESET) == 0;
    }

    // ---------------------------------------------------------------- NVRAM
    // The board description the firmware reads at boot: the text file with
    // comments and blank lines removed, each "name=value" NUL-terminated,
    // padded to four bytes, at the very top of RAM; the last word of RAM
    // holds its length in words, and the complement of that.
    alignas (4) std::uint8_t g_nvram[4096];

    std::size_t
    nvram_pack (const std::uint8_t* text, std::size_t size) noexcept
    {
      std::size_t out = 0;
      std::size_t at = 0;
      while (at < size)
        {
          std::size_t end = at;
          while (end < size && text[end] != '\n' && text[end] != '\r')
            {
              ++end;
            }
          const bool keep = end > at && text[at] != '#' && text[at] != ' '
                            && std::memchr (text + at, '=', end - at) != nullptr;
          if (keep && out + (end - at) + 8 < sizeof (g_nvram))
            {
              std::memcpy (g_nvram + out, text + at, end - at);
              out += end - at;
              g_nvram[out++] = 0;
            }
          at = end + 1;
        }
      g_nvram[out++] = 0;
      while ((out & 3u) != 0)
        {
          g_nvram[out++] = 0;
        }
      return out;
    }

    // ---------------------------------------------------------------- SDPCM
    bool send_frame (std::uint8_t channel, std::size_t header_size,
                     std::size_t total) noexcept;
    bool receive_one () noexcept;

    bool
    credit_available () noexcept
    {
      const std::uint8_t window = static_cast<std::uint8_t> (g_tx_credit - g_tx_seq);
      return window != 0 && (window & 0x80) == 0;
    }

    // g_tx holds the frame from byte 0; fill the SDPCM header and send.
    bool
    send_frame (std::uint8_t channel, std::size_t header_size,
                std::size_t total) noexcept
    {
      const std::uint64_t deadline = platform::time_us () + 500000u;
      while (!credit_available ())
        {
          if (!receive_one ())
            {
              platform::delay_us (200);
            }
          if (platform::time_us () > deadline)
            {
              return false;
            }
        }
      put16 (g_tx, static_cast<std::uint16_t> (total));
      put16 (g_tx + 2, static_cast<std::uint16_t> (~total));
      g_tx[4] = g_tx_seq++;
      g_tx[5] = channel;
      g_tx[6] = 0;
      g_tx[7] = static_cast<std::uint8_t> (header_size);
      g_tx[8] = 0;
      g_tx[9] = 0;
      g_tx[10] = 0;
      g_tx[11] = 0;
      // Up to one block goes in byte mode (a multiple of 4); more as whole
      // blocks. The padding is zeroed: it is on the wire.
      std::size_t wire = total <= sdio::block_size
                             ? ((total + 3u) & ~std::size_t{ 3 })
                             : ((total + sdio::block_size - 1u)
                                & ~(sdio::block_size - 1u));
      if (wire > sizeof (g_tx))
        {
          return false;
        }
      std::memset (g_tx + total, 0, wire - total);
      return sdio::write (2, 0, g_tx, wire);
    }

    void
    link_changed (bool up) noexcept
    {
      if (up == g_linked)
        {
          return;
        }
      g_linked = up;
      if (g_on_link != nullptr)
        {
          g_on_link (up);
        }
    }

    void
    handle_scan_result (const std::uint8_t* data, std::size_t size,
                        std::uint32_t status) noexcept
    {
      if (status != 8) // 8: partial result; anything else ends the scan
        {
          g_scan_done = true;
          return;
        }
      if (size < 12 + 80)
        {
          return;
        }
      const std::uint8_t* bss = data + 12;
      const std::size_t ssid_len = bss[18] <= 32 ? bss[18] : 32;
      network found{};
      std::memcpy (found.ssid, bss + 19, ssid_len);
      std::memcpy (found.bssid.data (), bss + 8, 6);
      found.rssi = static_cast<std::int16_t> (le16 (bss + 78));
      found.channel = static_cast<std::uint8_t> (le16 (bss + 72) & 0xff);
      found.secured = (le16 (bss + 16) & 0x0010) != 0; // capability: privacy
      // The information elements say which security: RSN (48) is WPA2, the
      // Microsoft vendor element 00-50-F2-01 is WPA; both list the pairwise
      // ciphers (suite type 2: TKIP, 4: CCMP) after the group cipher.
      const std::size_t bss_size = size - 12;
      const std::size_t ie_at = le16 (bss + 116);
      const std::size_t ie_size = le32 (bss + 120);
      if (bss_size >= 124 && ie_at + ie_size <= bss_size)
        {
          const std::uint8_t* ie = bss + ie_at;
          const std::uint8_t* const end = ie + ie_size;
          while (ie + 2 <= end && ie + 2 + ie[1] <= end)
            {
              const std::uint8_t* body = ie + 2;
              std::size_t n = ie[1];
              bool security = false;
              if (ie[0] == 48 && n >= 2)
                {
                  found.wpa2 = true;
                  security = true;
                }
              else if (ie[0] == 221 && n >= 6 && body[0] == 0x00 && body[1] == 0x50
                       && body[2] == 0xf2 && body[3] == 0x01)
                {
                  found.wpa = true;
                  security = true;
                  body += 4;
                  n -= 4;
                }
              // version (2), group suite (4), pairwise count (2), suites.
              if (security && n >= 8)
                {
                  const std::size_t count = le16 (body + 6);
                  for (std::size_t i = 0; i < count && 8 + 4 * (i + 1) <= n; ++i)
                    {
                      const std::uint8_t type = body[8 + 4 * i + 3];
                      found.tkip = found.tkip || type == 2;
                      found.ccmp = found.ccmp || type == 4;
                    }
                }
              ie += 2 + ie[1];
            }
        }
      if (ssid_len == 0)
        {
          return; // hidden network
        }
      for (std::size_t i = 0; i < g_network_count; ++i)
        {
          if (std::strcmp (g_networks[i].ssid, found.ssid) == 0)
            {
              if (found.rssi > g_networks[i].rssi)
                {
                  g_networks[i] = found;
                }
              return;
            }
        }
      if (g_network_count < scan_max)
        {
          g_networks[g_network_count++] = found;
        }
    }

    // An event: Ethernet header (type 0x886c), a 10-byte Broadcom header,
    // then the big-endian event message and its data.
    void
    handle_event (const std::uint8_t* frame, std::size_t size) noexcept
    {
      constexpr std::size_t message_at = 14 + 10;
      constexpr std::size_t message_size = 48;
      if (size < message_at + message_size)
        {
          return;
        }
      const std::uint8_t* message = frame + message_at;
      const std::uint16_t flags = static_cast<std::uint16_t> ((message[2] << 8) | message[3]);
      const std::uint32_t type = be32 (message + 4);
      const std::uint32_t status = be32 (message + 8);
      const std::uint32_t reason = be32 (message + 12);
      const std::uint32_t data_size = be32 (message + 20);
      const std::uint8_t* data = message + message_size;
      const std::size_t available = size - message_at - message_size;
      ++g_stats.rx_events;
      if (type != EV_ESCAN_RESULT)
        {
          g_ev_last = type;
          g_ev_last_status = status;
          g_ev_last_reason = reason;
        }

      switch (type)
        {
        case EV_ESCAN_RESULT:
          handle_scan_result (data, data_size < available ? data_size : available,
                              status);
          break;
        case EV_SET_SSID:
          g_ev_ssid_done = true;
          g_ev_ssid_status = status;
          break;
        case EV_LINK:
          g_ev_link_up = (flags & 1u) != 0;
          if (!g_ev_link_up)
            {
              g_ev_keyed = false;
              link_changed (false);
            }
          break;
        case EV_PSK_SUP:
          // 6: the supplicant has installed the keys. Anything else while
          // joining is progress or a failure (a wrong passphrase ends here).
          if (status == 6)
            {
              g_ev_keyed = true;
            }
          else if (status > 6)
            {
              g_ev_auth_failed = true;
            }
          break;
        case EV_DEAUTH:
        case EV_DEAUTH_IND:
        case EV_DISASSOC:
        case EV_DISASSOC_IND:
          g_ev_link_up = false;
          g_ev_keyed = false;
          link_changed (false);
          break;
        default:
          break;
        }
    }

    // Read one SDPCM frame from function 2, if there is one, and act on it.
    bool
    receive_one () noexcept
    {
      if (!sdio::read (2, 0, g_rx, 4))
        {
          ++g_stats.rx_errors;
          return false;
        }
      const std::uint16_t length = le16 (g_rx);
      const std::uint16_t check = le16 (g_rx + 2);
      if (length == 0 && check == 0)
        {
          return false; // nothing waiting
        }
      if (static_cast<std::uint16_t> (length ^ check) != 0xffff || length < sdpcm_header
          || length > sizeof (g_rx))
        {
          ++g_stats.rx_errors;
          return false;
        }
      std::size_t rest = length - 4u;
      rest = rest <= sdio::block_size
                 ? ((rest + 3u) & ~std::size_t{ 3 })
                 : ((rest + sdio::block_size - 1u) & ~(sdio::block_size - 1u));
      if (rest + 4u > sizeof (g_rx) || !sdio::read (2, 0, g_rx + 4, rest))
        {
          ++g_stats.rx_errors;
          return false;
        }
      ++g_stats.rx_frames;

      const std::uint8_t channel = g_rx[5] & 0x0f;
      const std::size_t header = g_rx[7];
      g_tx_credit = g_rx[9];
      if (header < sdpcm_header || header > length)
        {
          return true; // a credit update, or nothing we understand
        }
      const std::uint8_t* body = g_rx + header;
      const std::size_t body_size = length - header;

      if (channel == 0)
        {
          if (body_size < cdc_header)
            {
              return true;
            }
          const std::uint32_t flags = le32 (body + 8);
          g_reply_id = static_cast<std::uint16_t> (flags >> 16);
          g_reply_status = (flags & 1u) != 0 ? le32 (body + 12) : 0;
          g_reply_size = body_size - cdc_header;
          std::memcpy (g_reply, body + cdc_header, g_reply_size);
          g_reply_seen = true;
          return true;
        }

      // Event and data frames carry a 4-byte BDC header, whose last byte is
      // the offset to the Ethernet frame, in words.
      if (body_size < 4)
        {
          return true;
        }
      const std::size_t skip = 4u + 4u * body[3];
      if (body_size < skip + 14u)
        {
          return true;
        }
      const std::uint8_t* frame = body + skip;
      const std::size_t frame_size = body_size - skip;
      const bool event = frame[12] == 0x88 && frame[13] == 0x6c;
      if (event)
        {
          handle_event (frame, frame_size);
        }
      else if (channel == 2)
        {
          ++g_stats.rx_data;
          if (frame_size <= pending_frame && g_pending_count < pending_max)
            {
              const std::size_t slot
                  = (g_pending_head + g_pending_count) % pending_max;
              std::memcpy (g_pending[slot], frame, frame_size);
              g_pending_size[slot] = static_cast<std::uint16_t> (frame_size);
              ++g_pending_count;
            }
          else
            {
              ++g_pending_dropped;
              ++g_stats.rx_errors;
            }
        }
      return true;
    }

    // ------------------------------------------------------ control channel
    // One request, one response with the same id. `data` is both the
    // request payload and where the response is copied.
    bool
    ioctl (std::uint32_t command, bool set, void* data, std::size_t size,
           std::uint32_t timeout_ms = 2000) noexcept
    {
      const std::size_t total = sdpcm_header + cdc_header + size;
      if (total > sizeof (g_tx))
        {
          return false;
        }
      ++g_stats.ioctls;
      const std::uint16_t id = ++g_ioctl_id;
      std::uint8_t* cdc = g_tx + sdpcm_header;
      put32 (cdc, command);
      put32 (cdc + 4, static_cast<std::uint32_t> (size));
      put32 (cdc + 8, (static_cast<std::uint32_t> (id) << 16) | (set ? 2u : 0u));
      put32 (cdc + 12, 0);
      if (size != 0)
        {
          std::memcpy (cdc + cdc_header, data, size);
        }
      g_reply_seen = false;
      if (!send_frame (0, sdpcm_header, total))
        {
          ++g_stats.ioctl_errors;
          return false;
        }
      const std::uint64_t deadline
          = platform::time_us () + static_cast<std::uint64_t> (timeout_ms) * 1000u;
      for (;;)
        {
          if (!receive_one ())
            {
              platform::delay_us (200);
            }
          if (g_reply_seen && g_reply_id == id)
            {
              break;
            }
          g_reply_seen = false;
          if (platform::time_us () > deadline)
            {
              ++g_stats.ioctl_errors;
              return false;
            }
        }
      if (g_reply_status != 0)
        {
          ++g_stats.ioctl_errors;
          return false;
        }
      if (!set && size != 0)
        {
          std::memcpy (data, g_reply, g_reply_size < size ? g_reply_size : size);
        }
      return true;
    }

    bool
    ioctl_set_u32 (std::uint32_t command, std::uint32_t value) noexcept
    {
      std::uint8_t raw[4];
      put32 (raw, value);
      return ioctl (command, true, raw, sizeof (raw));
    }

    alignas (8) std::uint8_t g_var[1536];

    // "name\0" followed by the value.
    bool
    iovar_set (const char* name, const void* value, std::size_t size) noexcept
    {
      const std::size_t name_size = std::strlen (name) + 1;
      if (name_size + size > sizeof (g_var))
        {
          return false;
        }
      std::memcpy (g_var, name, name_size);
      if (size != 0)
        {
          std::memcpy (g_var + name_size, value, size);
        }
      return ioctl (WLC_SET_VAR, true, g_var, name_size + size);
    }

    bool
    iovar_set_u32 (const char* name, std::uint32_t value) noexcept
    {
      std::uint8_t raw[4];
      put32 (raw, value);
      return iovar_set (name, raw, sizeof (raw));
    }

    // Per-interface variables: "bsscfg:name\0", the interface index, the value.
    bool
    bsscfg_set_u32 (const char* name, std::uint32_t value) noexcept
    {
      std::uint8_t raw[8];
      put32 (raw, 0);
      put32 (raw + 4, value);
      return iovar_set (name, raw, sizeof (raw));
    }

    bool
    iovar_get (const char* name, void* value, std::size_t size) noexcept
    {
      const std::size_t name_size = std::strlen (name) + 1;
      const std::size_t total = name_size > size ? name_size : size;
      if (total > sizeof (g_var))
        {
          return false;
        }
      std::memset (g_var, 0, total);
      std::memcpy (g_var, name, name_size);
      if (!ioctl (WLC_GET_VAR, false, g_var, total))
        {
          return false;
        }
      std::memcpy (value, g_var, size);
      return true;
    }

    // The regulatory data (CLM), in pieces, through the "clmload" variable.
    bool
    clm_load (const std::uint8_t* clm, std::size_t size) noexcept
    {
      constexpr std::size_t piece_max = 1024;
      alignas (4) static std::uint8_t request[12 + piece_max];
      for (std::size_t at = 0; at < size;)
        {
          const std::size_t n = size - at < piece_max ? size - at : piece_max;
          std::uint16_t flag = 0x1000; // download handler version 1
          if (at == 0)
            {
              flag |= 0x0002; // begin
            }
          if (at + n == size)
            {
              flag |= 0x0004; // end
            }
          put16 (request, flag);
          put16 (request + 2, 2); // type: CLM
          put32 (request + 4, static_cast<std::uint32_t> (n));
          put32 (request + 8, 0);
          std::memcpy (request + 12, clm + at, n);
          if (!iovar_set ("clmload", request, 12 + ((n + 7u) & ~std::size_t{ 7 })))
            {
              return false;
            }
          at += n;
        }
      std::uint32_t status = 1;
      std::uint8_t raw[4] = {};
      if (iovar_get ("clmload_status", raw, sizeof (raw)))
        {
          status = le32 (raw);
        }
      return status == 0;
    }

    // ------------------------------------------------------------- bring-up
    bool
    chip_start () noexcept
    {
      // Function 1 on, then the chip's always-available clock.
      if (!sdio::write8 (0, 0x02, 0x02))
        {
          return fail ("cannot enable SDIO function 1");
        }
      std::uint8_t value = 0;
      for (unsigned i = 0; i < 200; ++i)
        {
          if (sdio::read8 (0, 0x03, value) && (value & 0x02) != 0)
            {
              break;
            }
          platform::delay_us (1000);
        }
      if ((value & 0x02) == 0)
        {
          return fail ("SDIO function 1 not ready");
        }
      if (!sdio::write8 (1, F1_CHIPCLKCSR, CSR_ALP_AVAIL_REQ))
        {
          return fail ("cannot request the ALP clock");
        }
      value = 0;
      for (unsigned i = 0; i < 200; ++i)
        {
          if (sdio::read8 (1, F1_CHIPCLKCSR, value) && (value & CSR_ALP_AVAIL) != 0)
            {
              break;
            }
          platform::delay_us (1000);
        }
      if ((value & CSR_ALP_AVAIL) == 0)
        {
          return fail ("the chip's ALP clock did not start");
        }
      (void)sdio::write8 (1, F1_CHIPCLKCSR, 0);
      (void)sdio::write8 (1, F1_SDIOPULLUP, 0);

      std::uint32_t id = 0;
      if (!bp_read32 (CHIPCOMMON, id))
        {
          return fail ("cannot read the chip id through the backplane");
        }
      g_chip = id & 0xffffu;
      g_chip_rev = (id >> 16) & 0x0fu;
      return true;
    }

    bool
    firmware_start () noexcept
    {
      // Which image: the Zero 2 W was built with two variants of the chip.
      const std::uint8_t* firmware = nullptr;
      std::size_t firmware_size = 0;
      const std::uint8_t* nvram = nullptr;
      std::size_t nvram_size = 0;
      if (g_chip != 43430)
        {
          std::snprintf (g_error_text, sizeof (g_error_text),
                         "unsupported chip %u rev %u", g_chip, g_chip_rev);
          return fail (g_error_text);
        }
      // Raspberry Pi's own mapping for this board (the Linux firmware names
      // "brcmfmac43430-sdio.raspberrypi,model-zero-2-w" and "...43430b0..."):
      // silicon revision 1 is the Synaptics variant and runs the 43436s
      // image; revision 2 and later run the 43436 image, with a CLM blob.
      if (g_chip_rev <= 1)
        {
          g_fw_name = "43436s";
          firmware = cyw43_fw_43436s;
          firmware_size = static_cast<std::size_t> (cyw43_fw_43436s_end - cyw43_fw_43436s);
          nvram = cyw43_nvram_43436s;
          nvram_size = static_cast<std::size_t> (cyw43_nvram_43436s_end - cyw43_nvram_43436s);
        }
      else
        {
          g_fw_name = "43436";
          firmware = cyw43_fw_43436;
          firmware_size = static_cast<std::size_t> (cyw43_fw_43436_end - cyw43_fw_43436);
          nvram = cyw43_nvram_43436;
          nvram_size = static_cast<std::size_t> (cyw43_nvram_43436_end - cyw43_nvram_43436);
        }

      // Halt the processor, wake the RAM.
      if (!core_disable (ARM_CORE) || !core_reset (SOCRAM_CORE))
        {
          return fail ("cannot reset the chip's cores");
        }
      (void)bp_write32 (SOCRAM_BANKX_INDEX, 3);
      (void)bp_write32 (SOCRAM_BANKX_PDA, 0);

      if (!bp_write_memory (ram_base, firmware, firmware_size))
        {
          return fail ("firmware download failed");
        }
      // The first and the last 64 bytes, read back.
      std::uint8_t check[64];
      // Word aligned: the backplane reads whole words.
      const std::size_t tail = (firmware_size - sizeof (check)) & ~std::size_t{ 3 };
      const bool head_read = bp_read_memory (ram_base, check, sizeof (check));
      const bool head_ok = head_read
                           && std::memcmp (check, firmware, sizeof (check)) == 0;
      if (!head_ok)
        {
          kprintf ("[WLAN] read-back, head: read %s; got %02x %02x %02x %02x "
                   "%02x %02x %02x %02x, want %02x %02x %02x %02x %02x %02x "
                   "%02x %02x\n",
                   head_read ? "ok" : sdio::last_error (), check[0], check[1],
                   check[2], check[3], check[4], check[5], check[6], check[7],
                   firmware[0], firmware[1], firmware[2], firmware[3],
                   firmware[4], firmware[5], firmware[6], firmware[7]);
        }
      const bool tail_read
          = bp_read_memory (ram_base + static_cast<std::uint32_t> (tail), check,
                            sizeof (check));
      const bool tail_ok
          = tail_read && std::memcmp (check, firmware + tail, sizeof (check)) == 0;
      if (!tail_ok)
        {
          kprintf ("[WLAN] read-back, tail @%u: read %s; got %02x %02x %02x "
                   "%02x %02x %02x %02x %02x, want %02x %02x %02x %02x %02x "
                   "%02x %02x %02x\n",
                   static_cast<unsigned> (tail),
                   tail_read ? "ok" : sdio::last_error (), check[0], check[1],
                   check[2], check[3], check[4], check[5], check[6], check[7],
                   firmware[tail], firmware[tail + 1], firmware[tail + 2],
                   firmware[tail + 3], firmware[tail + 4], firmware[tail + 5],
                   firmware[tail + 6], firmware[tail + 7]);
        }
      // Where the first difference is, in 64-byte steps, when one is bad.
      if (!head_ok || !tail_ok)
        {
          for (std::size_t at = 0; at + sizeof (check) <= firmware_size;
               at += 4096)
            {
              if (!bp_read_memory (ram_base + static_cast<std::uint32_t> (at),
                                   check, sizeof (check))
                  || std::memcmp (check, firmware + at, sizeof (check)) != 0)
                {
                  std::size_t first = 0;
                  while (first < sizeof (check)
                         && check[first] == firmware[at + first])
                    {
                      ++first;
                    }
                  kprintf ("[WLAN] first difference near %u (+%u): got %02x "
                           "want %02x\n",
                           static_cast<unsigned> (at),
                           static_cast<unsigned> (first), check[first % 64],
                           firmware[at + (first % 64)]);
                  break;
                }
            }
          return fail ("firmware read-back differs");
        }

      const std::size_t packed = nvram_pack (nvram, nvram_size);
      const std::uint32_t nvram_at
          = ram_base + ram_size - 4u - static_cast<std::uint32_t> (packed);
      const std::uint32_t words = static_cast<std::uint32_t> (packed / 4u);
      if (!bp_write_memory (nvram_at, g_nvram, packed)
          || !bp_write32 (ram_base + ram_size - 4u, (~words << 16) | (words & 0xffffu)))
        {
          return fail ("NVRAM download failed");
        }

      // Let the processor run the firmware.
      if (!core_reset (ARM_CORE) || !core_is_up (ARM_CORE))
        {
          return fail ("the chip's processor did not leave reset");
        }

      // The firmware announces itself by turning on the fast (HT) clock,
      // then by making function 2 ready.
      std::uint8_t value = 0;
      for (unsigned i = 0; i < 1000; ++i)
        {
          if (sdio::read8 (1, F1_CHIPCLKCSR, value) && (value & CSR_HT_AVAIL) != 0)
            {
              break;
            }
          platform::delay_us (1000);
        }
      if ((value & CSR_HT_AVAIL) == 0)
        {
          return fail ("firmware did not start (no HT clock)");
        }
      (void)bp_write32 (SDIO_INT_HOST_MASK, 0x000000f0u);
      if (!sdio::write8 (0, 0x02, 0x02 | 0x04))
        {
          return fail ("cannot enable SDIO function 2");
        }
      value = 0;
      for (unsigned i = 0; i < 1000; ++i)
        {
          if (sdio::read8 (0, 0x03, value) && (value & 0x04) != 0)
            {
              break;
            }
          platform::delay_us (1000);
        }
      if ((value & 0x04) == 0)
        {
          return fail ("firmware did not start (function 2 not ready)");
        }
      (void)sdio::write8 (1, F1_WATERMARK, 8);
      return true;
    }

    bool
    firmware_configure () noexcept
    {
      // Regulatory data, for the variant that keeps it in a separate file.
      if (std::strcmp (g_fw_name, "43436") == 0)
        {
          const std::size_t clm_size
              = static_cast<std::size_t> (cyw43_clm_43436_end - cyw43_clm_43436);
          if (!clm_load (cyw43_clm_43436, clm_size))
            {
              return fail ("CLM (regulatory data) download failed");
            }
        }

      (void)iovar_set_u32 ("bus:txglom", 0); // one frame per transfer
      (void)iovar_set_u32 ("ampdu_ba_wsize", 8);

      // country: abbreviation, revision (-1: default), code.
      std::uint8_t country[12] = {};
      std::memcpy (country, WIFI_COUNTRY, 2);
      put32 (country + 4, 0xffffffffu);
      std::memcpy (country + 8, WIFI_COUNTRY, 2);
      (void)iovar_set ("country", country, sizeof (country));

      // The events the driver follows; everything else stays off.
      std::uint8_t mask[16] = {};
      for (const std::uint32_t event :
           { EV_SET_SSID, EV_DEAUTH, EV_DEAUTH_IND, EV_DISASSOC, EV_DISASSOC_IND,
             EV_LINK, EV_PSK_SUP, EV_ESCAN_RESULT })
        {
          mask[event / 8u] |= static_cast<std::uint8_t> (1u << (event % 8u));
        }
      if (!iovar_set ("event_msgs", mask, sizeof (mask)))
        {
          return fail ("cannot set the event mask");
        }

      if (!iovar_get ("cur_etheraddr", g_address.data (), g_address.size ()))
        {
          return fail ("cannot read the MAC address");
        }
      if (iovar_get ("ver", g_fw_version, sizeof (g_fw_version) - 1))
        {
          for (char& c : g_fw_version)
            {
              if (c == '\n' || c == '\r')
                {
                  c = '\0';
                }
            }
        }

      (void)ioctl_set_u32 (WLC_SET_PM, 0); // no power save: the driver polls
      if (!ioctl_set_u32 (WLC_SET_INFRA, 1))
        {
          return fail ("cannot select infrastructure mode");
        }
      if (!ioctl (WLC_UP, true, nullptr, 0))
        {
          return fail ("cannot bring the interface up (WLC_UP)");
        }
      return true;
    }
  } // namespace

  bool
  start () noexcept
  {
    if (g_started)
      {
        return true;
      }
    if (!sdio::start ())
      {
        return fail (sdio::last_error ());
      }
    if (!chip_start () || !firmware_start () || !firmware_configure ())
      {
        return false;
      }
    g_started = true;
    return true;
  }

  std::span<const network>
  scan (std::uint32_t ms) noexcept
  {
    g_network_count = 0;
    g_scan_done = false;
    if (!g_started)
      {
        return {};
      }
    // escan: version 1, action "start", a sync id; then the scan parameters:
    // any SSID, any BSSID, any type, active, default times, all channels.
    std::uint8_t request[8 + 72] = {};
    put32 (request, 1);
    put16 (request + 4, 1);
    put16 (request + 6, 0x1234);
    std::uint8_t* params = request + 8;
    std::memset (params + 36, 0xff, 6); // BSSID: broadcast
    params[42] = 2;                     // BSS type: any
    params[43] = 0;                     // scan type: active
    put32 (params + 44, 0xffffffffu);   // probes per channel: default
    put32 (params + 48, 0xffffffffu);   // active dwell time: default
    put32 (params + 52, 0xffffffffu);   // passive dwell time: default
    put32 (params + 56, 0xffffffffu);   // home time: default
    put32 (params + 60, 0);             // channel count: all
    if (!iovar_set ("escan", request, sizeof (request)))
      {
        g_error = "the firmware refused the scan";
        return {};
      }
    const std::uint32_t start_ms = platform::millis ();
    while (!g_scan_done && platform::millis () - start_ms < ms)
      {
        if (!receive_one ())
          {
            platform::delay_us (1000);
          }
      }
    return { g_networks, g_network_count };
  }

  bool
  join (std::string_view ssid, std::string_view passphrase,
        std::uint32_t timeout_ms) noexcept
  {
    if (!g_started || ssid.empty () || ssid.size () > 32 || passphrase.size () > 64)
      {
        return fail ("join: bad arguments or radio not started");
      }
    g_ev_ssid_done = false;
    g_ev_link_up = false;
    g_ev_keyed = false;
    g_ev_auth_failed = false;
    g_ev_last = 0xffffffffu;
    const bool secured = !passphrase.empty ();

    // What the network advertised in the last scan decides the security;
    // a network that was not seen is taken to be WPA2 with AES.
    bool wpa2 = true;
    bool tkip = false;
    bool ccmp = true;
    for (std::size_t i = 0; i < g_network_count; ++i)
      {
        if (ssid == g_networks[i].name () && (g_networks[i].wpa || g_networks[i].wpa2))
          {
            wpa2 = g_networks[i].wpa2;
            tkip = g_networks[i].tkip;
            ccmp = g_networks[i].ccmp || !tkip;
          }
      }
    // wsec: bit 1 TKIP, bit 2 AES. wpa_auth: 0x04 WPA-PSK, 0x80 WPA2-PSK.
    const std::uint32_t wsec = secured ? (tkip ? 2u : 0u) | (ccmp ? 4u : 0u) : 0u;
    const std::uint32_t wpa_auth = secured ? (wpa2 ? 0x80u : 0x04u) : 0u;

    (void)ioctl (WLC_DISASSOC, true, nullptr, 0);
    bool ok = ioctl_set_u32 (WLC_SET_WSEC, wsec)
              && bsscfg_set_u32 ("bsscfg:sup_wpa", secured ? 1u : 0u);
    if (secured)
      {
        (void)bsscfg_set_u32 ("bsscfg:sup_wpa2_eapver", 0xffffffffu);
        (void)bsscfg_set_u32 ("bsscfg:sup_wpa_tmo", 2500);
        platform::delay_us (2000);
        // The passphrase; the firmware derives the key from it and the SSID.
        std::uint8_t pmk[68] = {};
        put16 (pmk, static_cast<std::uint16_t> (passphrase.size ()));
        put16 (pmk + 2, 1); // the key is a passphrase
        std::memcpy (pmk + 4, passphrase.data (), passphrase.size ());
        ok = ok && ioctl (WLC_SET_WSEC_PMK, true, pmk, sizeof (pmk));
      }
    ok = ok && ioctl_set_u32 (WLC_SET_INFRA, 1) && ioctl_set_u32 (WLC_SET_AUTH, 0)
         && ioctl_set_u32 (WLC_SET_WPA_AUTH, wpa_auth);
    if (!ok)
      {
        return fail ("join: the firmware refused the security settings");
      }

    // The requests above (the disassociation first of all) produce events of
    // their own, a failed "SSID set" among them. Let them arrive, then forget
    // them: only what follows the join request counts.
    for (const std::uint32_t settle = platform::millis ();
         platform::millis () - settle < 100u;)
      {
        if (!receive_one ())
          {
            platform::delay_us (1000);
          }
      }
    g_ev_ssid_done = false;
    g_ev_link_up = false;
    g_ev_keyed = false;
    g_ev_auth_failed = false;
    g_ev_last = 0xffffffffu;

    std::uint8_t request[36] = {};
    put32 (request, static_cast<std::uint32_t> (ssid.size ()));
    std::memcpy (request + 4, ssid.data (), ssid.size ());
    if (!ioctl (WLC_SET_SSID, true, request, sizeof (request)))
      {
        return fail ("join: the firmware refused the SSID");
      }

    const std::uint32_t start_ms = platform::millis ();
    for (;;)
      {
        if (!receive_one ())
          {
            platform::delay_us (1000);
          }
        if (g_ev_ssid_done && g_ev_ssid_status != 0)
          {
            std::snprintf (g_error_text, sizeof (g_error_text),
                           "join: network not found or refused (status %u)",
                           static_cast<unsigned> (g_ev_ssid_status));
            return fail (g_error_text);
          }
        if (g_ev_auth_failed)
          {
            return fail ("join: the WPA2 handshake failed (wrong passphrase?)");
          }
        if (g_ev_ssid_done && g_ev_link_up && (!secured || g_ev_keyed))
          {
            link_changed (true);
            return true;
          }
        if (platform::millis () - start_ms >= timeout_ms)
          {
            std::snprintf (g_error_text, sizeof (g_error_text),
                           "join: timeout (ssid %s, link %s, keys %s, last event "
                           "%u status %u reason %u)",
                           g_ev_ssid_done ? "set" : "pending",
                           g_ev_link_up ? "up" : "down", g_ev_keyed ? "yes" : "no",
                           static_cast<unsigned> (g_ev_last),
                           static_cast<unsigned> (g_ev_last_status),
                           static_cast<unsigned> (g_ev_last_reason));
            return fail (g_error_text);
          }
      }
  }

  void
  poll () noexcept
  {
    if (!g_started)
      {
        return;
      }
    // A bounded burst, so one busy network cannot starve the caller. Read
    // what the chip holds, then hand over what is waiting; the handler may
    // transmit, which may queue more.
    for (unsigned budget = 0; budget < 64; ++budget)
      {
        const bool read = g_pending_count < pending_max && receive_one ();
        if (g_pending_count != 0)
          {
            const std::size_t slot = g_pending_head;
            g_pending_head = (g_pending_head + 1) % pending_max;
            --g_pending_count;
            if (g_on_frame != nullptr)
              {
                g_on_frame ({ g_pending[slot], g_pending_size[slot] });
              }
          }
        else if (!read)
          {
            break;
          }
      }
  }

  bool
  send_ethernet (std::span<const std::uint8_t> frame) noexcept
  {
    // SDPCM header, two bytes of padding, the BDC header, the frame.
    constexpr std::size_t header = sdpcm_header + 2;
    const std::size_t total = header + 4 + frame.size ();
    if (!g_started || !g_linked || frame.size () < 14 || total > sizeof (g_tx))
      {
        ++g_stats.tx_errors;
        return false;
      }
    g_tx[12] = 0;
    g_tx[13] = 0;
    g_tx[14] = 0x20; // BDC version 2
    g_tx[15] = 0;    // priority
    g_tx[16] = 0;    // interface 0
    g_tx[17] = 0;    // no extra header words
    std::memcpy (g_tx + header + 4, frame.data (), frame.size ());
    if (!send_frame (2, header, total))
      {
        ++g_stats.tx_errors;
        return false;
      }
    ++g_stats.tx_data;
    return true;
  }

  void
  set_receive_handler (receive_handler handler) noexcept
  {
    g_on_frame = handler;
  }

  void
  set_link_handler (link_handler handler) noexcept
  {
    g_on_link = handler;
  }

  const mac_address&
  address () noexcept
  {
    return g_address;
  }

  bool
  linked () noexcept
  {
    return g_linked;
  }

  unsigned
  chip_id () noexcept
  {
    return g_chip;
  }

  unsigned
  chip_revision () noexcept
  {
    return g_chip_rev;
  }

  const char*
  firmware_name () noexcept
  {
    return g_fw_name;
  }

  const char*
  firmware_version () noexcept
  {
    return g_fw_version;
  }

  const statistics&
  stats () noexcept
  {
    return g_stats;
  }

  const char*
  last_error () noexcept
  {
    return g_error;
  }
} // namespace rpiz2w::wlan
