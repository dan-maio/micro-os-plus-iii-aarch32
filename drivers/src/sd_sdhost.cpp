/**
 * @file  sd_sdhost.cpp
 * @brief SD card on the BCM2835 SDHOST controller: polled, one block per
 *        command, 4-bit bus at default speed.
 *
 * The register values and the reset sequence follow the Linux driver
 * (drivers/mmc/host/bcm2835.c); no code is taken from it.
 */
#if defined(SD_BACKEND_SDHOST)

#include "sd.hpp"

#include <mailbox.hpp>
#include <timer_arm.hpp>

#include <cstring>

namespace sd
{
  namespace
  {
    constexpr std::uintptr_t base = 0x3F202000u;
    constexpr std::uint32_t SDCMD = 0x00;
    constexpr std::uint32_t SDARG = 0x04;
    constexpr std::uint32_t SDTOUT = 0x08;
    constexpr std::uint32_t SDCDIV = 0x0c;
    constexpr std::uint32_t SDRSP0 = 0x10;
    constexpr std::uint32_t SDHSTS = 0x20;
    constexpr std::uint32_t SDVDD = 0x30;
    constexpr std::uint32_t SDEDM = 0x34;
    constexpr std::uint32_t SDHCFG = 0x38;
    constexpr std::uint32_t SDHBCT = 0x3c;
    constexpr std::uint32_t SDDATA = 0x40;
    constexpr std::uint32_t SDHBLC = 0x50;

    constexpr std::uint32_t CMD_NEW = 0x8000;
    constexpr std::uint32_t CMD_FAIL = 0x4000;
    constexpr std::uint32_t CMD_BUSYWAIT = 0x0800;
    constexpr std::uint32_t CMD_NO_RESPONSE = 0x0400;
    constexpr std::uint32_t CMD_LONG_RESPONSE = 0x0200;
    constexpr std::uint32_t CMD_WRITE = 0x0080;
    constexpr std::uint32_t CMD_READ = 0x0040;

    constexpr std::uint32_t HSTS_DATA_FLAG = 0x01;
    constexpr std::uint32_t HSTS_FIFO_ERROR = 0x08;
    constexpr std::uint32_t HSTS_CRC7_ERROR = 0x10;
    constexpr std::uint32_t HSTS_CRC16_ERROR = 0x20;
    constexpr std::uint32_t HSTS_CMD_TIMEOUT = 0x40;
    constexpr std::uint32_t HSTS_REW_TIMEOUT = 0x80;
    constexpr std::uint32_t HSTS_ERRORS = HSTS_FIFO_ERROR | HSTS_CRC7_ERROR
                                          | HSTS_CRC16_ERROR | HSTS_CMD_TIMEOUT
                                          | HSTS_REW_TIMEOUT;
    constexpr std::uint32_t HSTS_CLEAR_ALL = 0x7f8;

    constexpr std::uint32_t HCFG_WIDE_INT_BUS = 0x02;
    constexpr std::uint32_t HCFG_WIDE_EXT_BUS = 0x04;
    constexpr std::uint32_t HCFG_SLOW_CARD = 0x08;

    constexpr std::uint32_t EDM_FSM_MASK = 0x0f;
    constexpr std::uint32_t EDM_FSM_IDENTMODE = 0x0;
    constexpr std::uint32_t EDM_FSM_DATAMODE = 0x1;
    constexpr std::uint32_t EDM_FSM_READWAIT = 0x4;
    constexpr std::uint32_t EDM_FSM_WRITESTART1 = 0xa;
    constexpr std::uint32_t EDM_FORCE_DATA_MODE = 1u << 19;

    // GPIO: the card slot's six pins, function select and pulls.
    constexpr std::uintptr_t gpio = 0x3F200000u;
    constexpr std::uint32_t GPPUD = 0x94;
    constexpr std::uint32_t GPPUDCLK1 = 0x9c;

    inline std::uint32_t
    rd (std::uint32_t offset) noexcept
    {
      return *reinterpret_cast<volatile std::uint32_t*> (base + offset);
    }

    inline void
    wr (std::uint32_t offset, std::uint32_t value) noexcept
    {
      *reinterpret_cast<volatile std::uint32_t*> (base + offset) = value;
    }

    inline std::uint32_t
    io_rd (std::uintptr_t address) noexcept
    {
      return *reinterpret_cast<volatile std::uint32_t*> (address);
    }

    inline void
    io_wr (std::uintptr_t address, std::uint32_t value) noexcept
    {
      *reinterpret_cast<volatile std::uint32_t*> (address) = value;
    }

    // Time from the ARM generic timer: this file has no RTOS dependency.
    // The counter is read through the port's CP15 accessor, and its rate
    // comes from the port's calibration -- CNTFRQ lies on this board (it
    // reports 19.2 MHz while the counter runs at about 1 MHz).
    std::uint64_t
    now_us () noexcept
    {
      const std::uint64_t count = timer_arm::get_count ();
      std::uint64_t hz = timer_arm::frequency ();
      if (hz == 0)
        {
          hz = 19200000u;
        }
      return (count / hz) * 1000000u + ((count % hz) * 1000000u) / hz;
    }

    void
    delay_us (std::uint32_t us) noexcept
    {
      const std::uint64_t until = now_us () + us;
      while (now_us () < until)
        {
        }
    }

    void
    route_pins () noexcept
    {
      // GPIO48-53 to ALT0 (function code 4): SDHOST CLK, CMD, DAT0-3.
      for (unsigned pin = 48; pin <= 53; ++pin)
        {
          const std::uintptr_t reg = gpio + (pin / 10u) * 4u;
          const unsigned shift = (pin % 10u) * 3u;
          io_wr (reg, (io_rd (reg) & ~(7u << shift)) | (4u << shift));
        }
      // Pull-ups on CMD and DAT0-3 (49-53), none on CLK (48).
      io_wr (gpio + GPPUD, 2);
      delay_us (200);
      io_wr (gpio + GPPUDCLK1, 0x1fu << (49 - 32));
      delay_us (200);
      io_wr (gpio + GPPUD, 0);
      io_wr (gpio + GPPUDCLK1, 0);
    }

    // The controller is clocked by the VideoCore "core" clock; ask the
    // firmware for its rate (clock id 4) rather than assume config.txt.
    std::uint32_t
    core_clock_hz () noexcept
    {
      alignas (16) static std::uint32_t request[8];
      request[0] = sizeof (request);
      request[1] = 0;
      request[2] = 0x00030002u; // get clock rate
      request[3] = 8;
      request[4] = 0;
      request[5] = 4; // core
      request[6] = 0;
      request[7] = 0;
      if (mailbox::property_call (request) && request[1] == mailbox::kRespSuccess
          && request[6] != 0)
        {
          return request[6];
        }
      return 250000000u;
    }

    void
    set_clock (std::uint32_t hz) noexcept
    {
      const std::uint32_t core = core_clock_hz ();
      std::uint32_t divider = core / hz;
      if (divider * hz < core)
        {
          ++divider; // never faster than asked
        }
      divider = divider >= 2 ? divider - 2 : 0;
      if (divider > 0x7ff)
        {
          divider = 0x7ff;
        }
      wr (SDCDIV, divider);
      // The read/erase/write timeout, in clock cycles: about half a second.
      wr (SDTOUT, (core / (divider + 2)) / 2);
    }

    std::uint32_t g_hcfg = 0;

    void
    reset () noexcept
    {
      wr (SDVDD, 0);
      wr (SDCMD, 0);
      wr (SDARG, 0);
      wr (SDTOUT, 0xf00000);
      wr (SDCDIV, 0);
      wr (SDHSTS, HSTS_CLEAR_ALL);
      wr (SDHCFG, 0);
      wr (SDHBCT, 0);
      wr (SDHBLC, 0);
      // FIFO thresholds of 4 words, for reading and for writing.
      std::uint32_t edm = rd (SDEDM);
      edm &= ~((0x1fu << 14) | (0x1fu << 9));
      edm |= (4u << 14) | (4u << 9);
      wr (SDEDM, edm);
      delay_us (20000);
      wr (SDVDD, 1);
      delay_us (20000);
      g_hcfg = HCFG_SLOW_CARD | HCFG_WIDE_INT_BUS;
      wr (SDHCFG, g_hcfg);
    }

    // Bits [msb:lsb] of a 128-bit response held as four words, word 0 being
    // bits 31:0.
    std::uint32_t
    bits (const std::uint32_t* words, unsigned msb, unsigned lsb) noexcept
    {
      std::uint32_t value = 0;
      for (unsigned bit = msb + 1; bit-- > lsb;)
        {
          value = (value << 1) | ((words[bit / 32u] >> (bit % 32u)) & 1u);
        }
      return value;
    }
  } // namespace

  bool
  SdCard::fail (Result result, const char* message)
  {
    result_ = result;
    err_ = message;
    return false;
  }

  bool
  SdCard::command (std::uint32_t index, std::uint32_t argument,
                   std::uint32_t flags, bool ignore_crc)
  {
    std::uint64_t deadline = now_us () + 100000u;
    while ((rd (SDCMD) & CMD_NEW) != 0)
      {
        if (now_us () > deadline)
          {
            return fail (Result::err_timeout, "previous command still running");
          }
      }
    const std::uint32_t pending = rd (SDHSTS) & HSTS_ERRORS;
    if (pending != 0)
      {
        wr (SDHSTS, pending);
      }
    wr (SDARG, argument);
    wr (SDCMD, (index & 0x3f) | flags | CMD_NEW);
    deadline = now_us () + 1000000u;
    while ((rd (SDCMD) & CMD_NEW) != 0)
      {
        if (now_us () > deadline)
          {
            return fail (Result::err_timeout, "command did not complete");
          }
      }
    if ((rd (SDCMD) & CMD_FAIL) != 0)
      {
        const std::uint32_t status = rd (SDHSTS);
        wr (SDHSTS, status & HSTS_ERRORS);
        if ((status & HSTS_CMD_TIMEOUT) != 0)
          {
            return fail (Result::err_timeout, "no response to a command");
          }
        // A response without a CRC (R3, the operating conditions) always
        // fails the controller's CRC7 check.
        if (!(ignore_crc && (status & HSTS_CRC7_ERROR) != 0))
          {
            return fail (Result::err_cmd, "command failed");
          }
      }
    return true;
  }

  bool
  SdCard::app_command (std::uint32_t index, std::uint32_t argument,
                       std::uint32_t flags, bool ignore_crc)
  {
    return command (55, rca_ << 16, 0) && command (index, argument, flags, ignore_crc);
  }

  // After a write the card holds DAT0 low while it programs: ask for its
  // status until it is back in the transfer state and ready for data.
  bool
  SdCard::wait_ready ()
  {
    const std::uint64_t deadline = now_us () + 2000000u;
    for (;;)
      {
        if (command (13, rca_ << 16, 0))
          {
            const std::uint32_t status = rd (SDRSP0);
            if ((status & (1u << 8)) != 0 && ((status >> 9) & 0x0f) == 4)
              {
                return true;
              }
          }
        if (now_us () > deadline)
          {
            return fail (Result::err_timeout, "card stayed busy");
          }
      }
  }

  // The controller's state machine must be back at rest before the next
  // command; one state on each path needs a push to leave.
  bool
  SdCard::wait_fsm_idle (bool read)
  {
    const std::uint32_t alternate = read ? EDM_FSM_READWAIT : EDM_FSM_WRITESTART1;
    const std::uint64_t deadline = now_us () + 500000u;
    for (;;)
      {
        const std::uint32_t edm = rd (SDEDM);
        const std::uint32_t fsm = edm & EDM_FSM_MASK;
        if (fsm == EDM_FSM_IDENTMODE || fsm == EDM_FSM_DATAMODE)
          {
            return true;
          }
        if (fsm == alternate)
          {
            wr (SDEDM, edm | EDM_FORCE_DATA_MODE);
            return true;
          }
        if (now_us () > deadline)
          {
            return fail (Result::err_timeout, "controller did not return to idle");
          }
      }
  }

  bool
  SdCard::block (bool read, std::uint32_t lba, std::uint8_t* data)
  {
    const std::uint32_t address = hc_ ? lba : lba * kSectorSize;
    wr (SDHBCT, kSectorSize);
    wr (SDHBLC, 1);
    if (!command (read ? 17 : 24, address, read ? CMD_READ : CMD_WRITE))
      {
        return false;
      }
    const std::uint64_t deadline = now_us () + 1000000u;
    for (std::uint32_t word = 0; word < kSectorSize / 4u; ++word)
      {
        for (;;)
          {
            const std::uint32_t status = rd (SDHSTS);
            if ((status & (HSTS_CRC16_ERROR | HSTS_FIFO_ERROR | HSTS_REW_TIMEOUT)) != 0)
              {
                wr (SDHSTS, status & HSTS_ERRORS);
                return fail ((status & HSTS_REW_TIMEOUT) != 0 ? Result::err_timeout
                                                              : Result::err_crc,
                             read ? "data read error" : "data write error");
              }
            if ((status & HSTS_DATA_FLAG) != 0)
              {
                break;
              }
            if (now_us () > deadline)
              {
                return fail (Result::err_timeout,
                             read ? "data read timeout" : "data write timeout");
              }
          }
        if (read)
          {
            const std::uint32_t value = rd (SDDATA);
            std::memcpy (data + word * 4u, &value, 4);
          }
        else
          {
            std::uint32_t value;
            std::memcpy (&value, data + word * 4u, 4);
            wr (SDDATA, value);
          }
      }
    if (!wait_fsm_idle (read))
      {
        return false;
      }
    const std::uint32_t status = rd (SDHSTS);
    if ((status & (HSTS_CRC16_ERROR | HSTS_FIFO_ERROR | HSTS_REW_TIMEOUT)) != 0)
      {
        wr (SDHSTS, status & HSTS_ERRORS);
        return fail (Result::err_crc, read ? "data read error at block end"
                                           : "data write error at block end");
      }
    return read ? true : wait_ready ();
  }

  bool
  SdCard::read_sectors (std::uint32_t lba, std::uint32_t count, void* dst)
  {
    if (!ready_)
      {
        return fail (Result::err_no_card, "card not initialised");
      }
    if (lba + count > sectors_ || lba + count < lba)
      {
        return fail (Result::err_range, "read beyond the end of the card");
      }
    auto* data = static_cast<std::uint8_t*> (dst);
    for (std::uint32_t i = 0; i < count; ++i)
      {
        // One retry: a CRC error on a long wire is not a dead card.
        if (!block (true, lba + i, data + i * kSectorSize)
            && !block (true, lba + i, data + i * kSectorSize))
          {
            return false;
          }
      }
    result_ = Result::ok;
    err_ = "no error";
    return true;
  }

  bool
  SdCard::write_sectors (std::uint32_t lba, std::uint32_t count, const void* src)
  {
    if (!ready_)
      {
        return fail (Result::err_no_card, "card not initialised");
      }
    if (lba + count > sectors_ || lba + count < lba)
      {
        return fail (Result::err_range, "write beyond the end of the card");
      }
    auto* data = const_cast<std::uint8_t*> (static_cast<const std::uint8_t*> (src));
    for (std::uint32_t i = 0; i < count; ++i)
      {
        if (!block (false, lba + i, data + i * kSectorSize))
          {
            return false;
          }
      }
    result_ = Result::ok;
    err_ = "no error";
    return true;
  }

  bool
  SdCard::init ()
  {
    ready_ = false;
    hc_ = false;
    rca_ = 0;
    route_pins ();
    reset ();
    set_clock (400000);
    delay_us (10000);

    // CMD0: idle. CMD8: a version 2 card echoes the check pattern.
    (void)command (0, 0, CMD_NO_RESPONSE);
    delay_us (2000);
    const bool v2 = command (8, 0x1aa, 0) && (rd (SDRSP0) & 0xfff) == 0x1aa;

    // ACMD41 until the card leaves busy; ask for high capacity on v2 cards.
    bool powered = false;
    for (unsigned i = 0; i < 1000 && !powered; ++i)
      {
        if (!app_command (41, 0x00ff8000u | (v2 ? 0x40000000u : 0u), 0, true))
          {
            if (i > 5)
              {
                return fail (Result::err_no_card, "no SD card answered ACMD41");
              }
          }
        else if ((rd (SDRSP0) & 0x80000000u) != 0)
          {
            powered = true;
            hc_ = (rd (SDRSP0) & 0x40000000u) != 0;
          }
        if (!powered)
          {
            delay_us (2000);
          }
      }
    if (!powered)
      {
        return fail (Result::err_voltage, "card did not finish power-up");
      }

    // CMD2: identification (ignored). CMD3: the relative address.
    if (!command (2, 0, CMD_LONG_RESPONSE) || !command (3, 0, 0))
      {
        return fail (Result::err_cmd, "card identification failed");
      }
    rca_ = rd (SDRSP0) >> 16;

    // CMD9: the CSD, for the capacity.
    if (!command (9, rca_ << 16, CMD_LONG_RESPONSE))
      {
        return fail (Result::err_cmd, "cannot read the CSD");
      }
    std::uint32_t csd[4];
    for (unsigned i = 0; i < 4; ++i)
      {
        csd[i] = rd (SDRSP0 + 4u * i);
      }
    const std::uint32_t structure = bits (csd, 127, 126);
    if (structure == 1)
      {
        sectors_ = (bits (csd, 69, 48) + 1u) * 1024u;
      }
    else if (structure == 0)
      {
        const std::uint32_t c_size = bits (csd, 73, 62);
        const std::uint32_t mult = bits (csd, 49, 47);
        const std::uint32_t read_bl_len = bits (csd, 83, 80);
        sectors_ = ((c_size + 1u) << (mult + 2u)) << (read_bl_len - 9u);
      }
    else
      {
        return fail (Result::err_unsupported, "unknown CSD structure");
      }

    // CMD7: select the card. ACMD6: four data lines. CMD16: 512-byte blocks.
    if (!command (7, rca_ << 16, CMD_BUSYWAIT))
      {
        return fail (Result::err_cmd, "cannot select the card");
      }
    if (app_command (6, 2, 0))
      {
        g_hcfg |= HCFG_WIDE_EXT_BUS;
        wr (SDHCFG, g_hcfg);
      }
    if (!hc_ && !command (16, kSectorSize, 0))
      {
        return fail (Result::err_cmd, "cannot set the block length");
      }

    set_clock (25000000);
    delay_us (2000);
    ready_ = true;
    result_ = Result::ok;
    err_ = "no error";
    return true;
  }
} // namespace sd

#endif // SD_BACKEND_SDHOST
