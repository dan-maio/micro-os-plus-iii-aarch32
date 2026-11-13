// sdio.cpp - SDIO host on the BCM2837 Arasan SDHCI, polled, 32-bit accesses.
//
// Board facts (Raspberry Pi Zero 2 W, the same on the Pi 3 family):
//   GPIO34-39  SD1 CLK, CMD, DAT0-3 to the Wi-Fi chip        (ALT3)
//   GPIO41     WL_ON, the chip's power enable, active high
//   GPIO43     32.768 kHz low-power clock for the chip, GPCLK2 (ALT0)
//   GPIO48-53  the SD card slot: ALT3 = this controller, ALT0 = SDHOST
#include "sdio.hpp"

#include "platform.hpp"

#include <bcm2837.hpp>

namespace rpiz2w::sdio
{
  namespace
  {
    // ------------------------------------------------------------ registers
    constexpr std::uintptr_t emmc = bcm::PERIPH_BASE + 0x300000u;
    constexpr std::uint32_t BLKSIZECNT = 0x04;
    constexpr std::uint32_t ARG1 = 0x08;
    constexpr std::uint32_t CMDTM = 0x0c;
    constexpr std::uint32_t RESP0 = 0x10;
    constexpr std::uint32_t DATA = 0x20;
    constexpr std::uint32_t STATUS = 0x24;
    constexpr std::uint32_t CONTROL0 = 0x28;
    constexpr std::uint32_t CONTROL1 = 0x2c;
    constexpr std::uint32_t INTERRUPT = 0x30;
    constexpr std::uint32_t IRPT_MASK = 0x34;
    constexpr std::uint32_t IRPT_EN = 0x38;

    constexpr std::uint32_t STATUS_CMD_INHIBIT = 1u << 0;
    constexpr std::uint32_t STATUS_DAT_INHIBIT = 1u << 1;

    constexpr std::uint32_t C0_4BIT = 1u << 1;
    constexpr std::uint32_t C0_POWER_3V3 = 0x0fu << 8;

    constexpr std::uint32_t C1_CLK_INTLEN = 1u << 0;
    constexpr std::uint32_t C1_CLK_STABLE = 1u << 1;
    constexpr std::uint32_t C1_CLK_EN = 1u << 2;
    constexpr std::uint32_t C1_TIMEOUT_MAX = 0x0eu << 16;
    constexpr std::uint32_t C1_SRST_HC = 1u << 24;
    constexpr std::uint32_t C1_SRST_CMD = 1u << 25;
    constexpr std::uint32_t C1_SRST_DATA = 1u << 26;

    constexpr std::uint32_t INT_CMD_DONE = 1u << 0;
    constexpr std::uint32_t INT_DATA_DONE = 1u << 1;
    constexpr std::uint32_t INT_WRITE_RDY = 1u << 4;
    constexpr std::uint32_t INT_READ_RDY = 1u << 5;
    constexpr std::uint32_t INT_ERROR = 1u << 15;
    constexpr std::uint32_t INT_ERROR_MASK = 0xffff0000u | INT_ERROR;

    constexpr std::uint32_t TM_BLKCNT_EN = 1u << 1;
    constexpr std::uint32_t TM_DAT_READ = 1u << 4;
    constexpr std::uint32_t TM_MULTI_BLOCK = 1u << 5;
    constexpr std::uint32_t CMD_RSPNS_48 = 2u << 16;
    constexpr std::uint32_t CMD_RSPNS_48_BUSY = 3u << 16;
    constexpr std::uint32_t CMD_CRCCHK_EN = 1u << 19;
    constexpr std::uint32_t CMD_IXCHK_EN = 1u << 20;
    constexpr std::uint32_t CMD_ISDATA = 1u << 21;

    // GPIO and the general-purpose clock.
    constexpr std::uintptr_t gpio = bcm::GPIO_BASE;
    constexpr std::uint32_t GPFSEL3 = 0x0c;
    constexpr std::uint32_t GPFSEL4 = 0x10;
    constexpr std::uint32_t GPFSEL5 = 0x14;
    constexpr std::uint32_t GPSET1 = 0x20;
    constexpr std::uint32_t GPCLR1 = 0x2c;
    constexpr std::uint32_t GPPUD = 0x94;
    constexpr std::uint32_t GPPUDCLK1 = 0x9c;
    constexpr std::uint32_t FSEL_OUTPUT = 1;
    constexpr std::uint32_t FSEL_ALT0 = 4;
    constexpr std::uint32_t FSEL_ALT3 = 7;

    constexpr std::uintptr_t CM_GP2CTL = bcm::PERIPH_BASE + 0x101080u;
    constexpr std::uintptr_t CM_GP2DIV = bcm::PERIPH_BASE + 0x101084u;
    constexpr std::uint32_t CM_PASSWORD = 0x5a000000u;

    // ---------------------------------------------------------------- state
    const char* g_error = "none";
    std::uint32_t g_ocr = 0;
    std::uint32_t g_rca = 0;
    // The controller loses a register write that follows another within two
    // SD clock periods. That matters at the 400 kHz identification clock.
    std::uint32_t g_write_delay_us = 6;

    inline std::uint32_t
    mmio_rd (std::uintptr_t address) noexcept
    {
      return *reinterpret_cast<volatile std::uint32_t*> (address);
    }

    inline void
    mmio_wr (std::uintptr_t address, std::uint32_t value) noexcept
    {
      *reinterpret_cast<volatile std::uint32_t*> (address) = value;
    }

    inline std::uint32_t
    rd (std::uint32_t offset) noexcept
    {
      return mmio_rd (emmc + offset);
    }

    inline void
    wr (std::uint32_t offset, std::uint32_t value) noexcept
    {
      mmio_wr (emmc + offset, value);
      if (g_write_delay_us != 0)
        {
          const std::uint64_t until = platform::time_us () + g_write_delay_us;
          while (platform::time_us () < until)
            {
            }
        }
    }

    void
    gpio_function (unsigned pin, std::uint32_t function) noexcept
    {
      const std::uint32_t reg = (pin / 10u) * 4u;
      const unsigned shift = (pin % 10u) * 3u;
      std::uint32_t value = mmio_rd (gpio + reg);
      value &= ~(7u << shift);
      value |= function << shift;
      mmio_wr (gpio + reg, value);
    }

    // BCM2835-style pull control: 0 off, 1 down, 2 up; pins 32-53.
    void
    gpio_pull_bank1 (std::uint32_t mask, std::uint32_t pull) noexcept
    {
      mmio_wr (gpio + GPPUD, pull);
      platform::delay_us (200);
      mmio_wr (gpio + GPPUDCLK1, mask);
      platform::delay_us (200);
      mmio_wr (gpio + GPPUD, 0);
      mmio_wr (gpio + GPPUDCLK1, 0);
    }

    void
    board_pins_and_power () noexcept
    {
      // The 32.768 kHz clock the chip sleeps on: GPCLK2 from the 19.2 MHz
      // oscillator, divided by 585.9375 (integer 585, fraction 3840/4096).
      mmio_wr (CM_GP2CTL, CM_PASSWORD | 1u); // source: oscillator, disabled
      for (unsigned i = 0; i < 1000 && (mmio_rd (CM_GP2CTL) & (1u << 7)) != 0; ++i)
        {
          platform::delay_us (10);
        }
      mmio_wr (CM_GP2DIV, CM_PASSWORD | (585u << 12) | 3840u);
      mmio_wr (CM_GP2CTL, CM_PASSWORD | (1u << 9) | 1u);          // MASH 1
      mmio_wr (CM_GP2CTL, CM_PASSWORD | (1u << 9) | (1u << 4) | 1u); // enable
      gpio_function (43, FSEL_ALT0);

      // Power the chip down and up, so it starts from reset whatever a
      // previous run left behind.
      gpio_function (41, FSEL_OUTPUT);
      mmio_wr (gpio + GPCLR1, 1u << (41 - 32));
      platform::delay_us (50000);

      // The card slot goes to the other controller (SDHOST), so this one is
      // connected to the radio alone.
      for (unsigned pin = 48; pin <= 53; ++pin)
        {
          gpio_function (pin, FSEL_ALT0);
        }
      for (unsigned pin = 34; pin <= 39; ++pin)
        {
          gpio_function (pin, FSEL_ALT3);
        }
      gpio_pull_bank1 (1u << (34 - 32), 0);    // CLK: no pull
      gpio_pull_bank1 (0x1fu << (35 - 32), 2); // CMD, DAT0-3: pull up

      mmio_wr (gpio + GPSET1, 1u << (41 - 32));
      platform::delay_us (150000);
    }

    // SD clock = base clock / (2 * divisor); the base is about 200 MHz.
    bool
    set_clock (std::uint32_t divisor) noexcept
    {
      std::uint32_t control = rd (CONTROL1);
      control &= ~C1_CLK_EN;
      wr (CONTROL1, control);
      control &= ~0x0000ffc0u;
      control |= (divisor & 0xffu) << 8;
      control |= ((divisor >> 8) & 0x3u) << 6;
      control |= C1_CLK_INTLEN | C1_TIMEOUT_MAX;
      wr (CONTROL1, control);
      for (unsigned i = 0; i < 10000; ++i)
        {
          if ((rd (CONTROL1) & C1_CLK_STABLE) != 0)
            {
              wr (CONTROL1, rd (CONTROL1) | C1_CLK_EN);
              platform::delay_us (2000);
              return true;
            }
          platform::delay_us (10);
        }
      g_error = "the SD clock did not stabilise";
      return false;
    }

    void
    recover (std::uint32_t reset_bits) noexcept
    {
      wr (CONTROL1, rd (CONTROL1) | reset_bits);
      for (unsigned i = 0; i < 10000 && (rd (CONTROL1) & reset_bits) != 0; ++i)
        {
          platform::delay_us (10);
        }
      wr (INTERRUPT, 0xffffffffu);
    }

    bool
    wait_interrupt (std::uint32_t wanted, std::uint32_t timeout_us) noexcept
    {
      const std::uint64_t deadline = platform::time_us () + timeout_us;
      for (;;)
        {
          const std::uint32_t status = rd (INTERRUPT);
          if ((status & INT_ERROR_MASK) != 0)
            {
              return false;
            }
          if ((status & wanted) != 0)
            {
              wr (INTERRUPT, wanted);
              return true;
            }
          if (platform::time_us () > deadline)
            {
              return false;
            }
        }
    }

    // Issue a command without data and return its 32-bit response.
    bool
    command (std::uint32_t index, std::uint32_t argument, std::uint32_t flags,
             std::uint32_t& response) noexcept
    {
      for (unsigned i = 0; (rd (STATUS) & STATUS_CMD_INHIBIT) != 0; ++i)
        {
          if (i > 100000)
            {
              g_error = "command line busy";
              recover (C1_SRST_CMD);
              return false;
            }
          platform::delay_us (1);
        }
      wr (INTERRUPT, 0xffffffffu);
      wr (ARG1, argument);
      wr (CMDTM, (index << 24) | flags);
      if (!wait_interrupt (INT_CMD_DONE, 100000))
        {
          g_error = "no command response";
          recover (C1_SRST_CMD);
          return false;
        }
      response = rd (RESP0);
      return true;
    }

    constexpr std::uint32_t r5_errors = 0xcb00u;

    bool
    transfer (bool write, unsigned fn, std::uint32_t address, std::uint8_t* data,
              std::size_t size) noexcept
    {
      if (size == 0)
        {
          return true;
        }
      const bool blocks = size > block_size;
      if (blocks && (size % block_size) != 0)
        {
          g_error = "block transfer not a multiple of the block size";
          return false;
        }
      const std::size_t count = blocks ? size / block_size : 1;
      const std::size_t each = blocks ? block_size : size;
      if (count > 511 || each > 512)
        {
          g_error = "transfer too large";
          return false;
        }

      for (unsigned i = 0;
           (rd (STATUS) & (STATUS_CMD_INHIBIT | STATUS_DAT_INHIBIT)) != 0; ++i)
        {
          if (i > 500000)
            {
              g_error = "data line busy";
              recover (C1_SRST_CMD | C1_SRST_DATA);
              return false;
            }
          platform::delay_us (1);
        }

      // IO_RW_EXTENDED: incrementing address; byte count, or block count.
      std::uint32_t argument = (write ? 1u << 31 : 0u) | (fn << 28) | (1u << 26)
                               | ((address & 0x1ffffu) << 9);
      argument |= blocks ? (1u << 27) | static_cast<std::uint32_t> (count)
                         : static_cast<std::uint32_t> (each & 0x1ffu);
      std::uint32_t flags = CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN
                            | CMD_ISDATA;
      if (!write)
        {
          flags |= TM_DAT_READ;
        }
      if (blocks)
        {
          flags |= TM_MULTI_BLOCK | TM_BLKCNT_EN;
        }

      wr (INTERRUPT, 0xffffffffu);
      wr (BLKSIZECNT, static_cast<std::uint32_t> (each)
                          | (static_cast<std::uint32_t> (count) << 16));
      wr (ARG1, argument);
      wr (CMDTM, (53u << 24) | flags);
      if (!wait_interrupt (INT_CMD_DONE, 100000))
        {
          g_error = "no CMD53 response";
          recover (C1_SRST_CMD | C1_SRST_DATA);
          return false;
        }
      if ((rd (RESP0) & r5_errors) != 0)
        {
          g_error = "CMD53 rejected by the card";
          recover (C1_SRST_CMD | C1_SRST_DATA);
          return false;
        }

      const std::size_t words = (each + 3u) / 4u;
      for (std::size_t block = 0; block < count; ++block)
        {
          if (!wait_interrupt (write ? INT_WRITE_RDY : INT_READ_RDY, 500000))
            {
              g_error = write ? "data write timeout" : "data read timeout";
              recover (C1_SRST_CMD | C1_SRST_DATA);
              return false;
            }
          for (std::size_t word = 0; word < words; ++word)
            {
              const std::size_t at = block * each + word * 4u;
              const std::size_t left = (block * each + each) - at;
              const std::size_t n = left < 4u ? left : 4u;
              if (write)
                {
                  std::uint32_t value = 0;
                  for (std::size_t b = 0; b < n; ++b)
                    {
                      value |= static_cast<std::uint32_t> (data[at + b]) << (8u * b);
                    }
                  // The data port takes no settling delay: it is a FIFO.
                  mmio_wr (emmc + DATA, value);
                }
              else
                {
                  const std::uint32_t value = rd (DATA);
                  for (std::size_t b = 0; b < n; ++b)
                    {
                      data[at + b] = static_cast<std::uint8_t> (value >> (8u * b));
                    }
                }
            }
        }
      if (!wait_interrupt (INT_DATA_DONE, 500000))
        {
          g_error = "data end timeout";
          recover (C1_SRST_CMD | C1_SRST_DATA);
          return false;
        }
      return true;
    }
  } // namespace

  bool
  read8 (unsigned fn, std::uint32_t address, std::uint8_t& value) noexcept
  {
    std::uint32_t response = 0;
    if (!command (52, (fn << 28) | ((address & 0x1ffffu) << 9),
                  CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, response))
      {
        return false;
      }
    if ((response & r5_errors) != 0)
      {
        g_error = "CMD52 read rejected by the card";
        return false;
      }
    value = static_cast<std::uint8_t> (response);
    return true;
  }

  bool
  write8 (unsigned fn, std::uint32_t address, std::uint8_t value) noexcept
  {
    std::uint32_t response = 0;
    if (!command (52, (1u << 31) | (fn << 28) | ((address & 0x1ffffu) << 9) | value,
                  CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, response))
      {
        return false;
      }
    if ((response & r5_errors) != 0)
      {
        g_error = "CMD52 write rejected by the card";
        return false;
      }
    return true;
  }

  bool
  read (unsigned fn, std::uint32_t address, void* data, std::size_t size) noexcept
  {
    return transfer (false, fn, address, static_cast<std::uint8_t*> (data), size);
  }

  bool
  write (unsigned fn, std::uint32_t address, const void* data,
         std::size_t size) noexcept
  {
    return transfer (true, fn, address,
                     const_cast<std::uint8_t*> (
                         static_cast<const std::uint8_t*> (data)),
                     size);
  }

  bool
  start () noexcept
  {
    board_pins_and_power ();

    // Reset the host and start the identification clock (about 400 kHz).
    g_write_delay_us = 6;
    wr (CONTROL1, C1_SRST_HC);
    for (unsigned i = 0; i < 10000 && (rd (CONTROL1) & C1_SRST_HC) != 0; ++i)
      {
        platform::delay_us (10);
      }
    wr (CONTROL0, C0_POWER_3V3);
    if (!set_clock (0x100))
      {
        return false;
      }
    wr (IRPT_EN, 0);           // polled: no interrupt line
    wr (IRPT_MASK, 0xffffffffu); // every status bit visible
    wr (INTERRUPT, 0xffffffffu);
    platform::delay_us (10000);

    // CMD5: an SDIO card answers with its operating conditions; then again,
    // naming 3.2-3.4 V, until it reports ready.
    std::uint32_t response = 0;
    bool ready = false;
    for (unsigned attempt = 0; attempt < 3 && !ready; ++attempt)
      {
        if (!command (5, 0, CMD_RSPNS_48, response))
          {
            platform::delay_us (20000);
            continue;
          }
        for (unsigned i = 0; i < 200; ++i)
          {
            if (!command (5, 0x00300000u, CMD_RSPNS_48, response))
              {
                break;
              }
            if ((response & 0x80000000u) != 0)
              {
                ready = true;
                break;
              }
            platform::delay_us (10000);
          }
      }
    if (!ready)
      {
        g_error = "no SDIO card answered CMD5 (is the chip powered?)";
        return false;
      }
    g_ocr = response;

    // CMD3: relative address; CMD7: select the card.
    if (!command (3, 0, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, response))
      {
        g_error = "CMD3 failed";
        return false;
      }
    g_rca = response >> 16;
    if (!command (7, g_rca << 16,
                  CMD_RSPNS_48_BUSY | CMD_CRCCHK_EN | CMD_IXCHK_EN, response))
      {
        g_error = "CMD7 failed";
        return false;
      }

    // Four data lines: first the card (CCCR bus interface control, with the
    // card-detect pull-up off), then the host.
    if (!write8 (0, 0x07, 0x80 | 0x02))
      {
        return false;
      }
    wr (CONTROL0, rd (CONTROL0) | C0_4BIT);

    // One block size for function 0, 1 and 2.
    for (unsigned fn = 0; fn <= 2; ++fn)
      {
        const std::uint32_t fbr = fn * 0x100u;
        if (!write8 (0, fbr + 0x10, static_cast<std::uint8_t> (block_size))
            || !write8 (0, fbr + 0x11, static_cast<std::uint8_t> (block_size >> 8)))
          {
            return false;
          }
      }

    // Default-speed data clock: base / 8 = 25 MHz.
    if (!set_clock (4))
      {
        return false;
      }
    g_write_delay_us = 0;
    return true;
  }

  const char*
  last_error () noexcept
  {
    return g_error;
  }

  std::uint32_t
  ocr () noexcept
  {
    return g_ocr;
  }

  std::uint32_t
  rca () noexcept
  {
    return g_rca;
  }
} // namespace rpiz2w::sdio
