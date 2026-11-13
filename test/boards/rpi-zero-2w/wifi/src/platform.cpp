// platform.cpp - time, delays, the console and its RAM copy.
#include "platform.hpp"

#include <cmsis-plus/rtos/os.h>
#include <timer_arm.hpp>
#include <uart.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rpiz2w::platform
{
  namespace
  {
    // The counter and its rate come from the port: the accessor is a CP15
    // read, and the rate is measured at start-up because CNTFRQ lies on this
    // board (19.2 MHz reported, about 1 MHz actual).
    inline std::uint64_t
    counter () noexcept
    {
      return timer_arm::get_count ();
    }

    inline std::uint64_t
    frequency () noexcept
    {
      const std::uint64_t value = timer_arm::frequency ();
      return value != 0 ? value : 19200000u; // the BCM2837 crystal
    }

    std::uint64_t g_origin = 0;

    // The RAM copy of the console. Text beyond the buffer is dropped: the
    // beginning of a bring-up is worth more than its end.
    char g_log[48 * 1024];
    std::size_t g_log_size = 0;
    std::size_t g_log_dumped = 0;
  } // namespace

  std::uint64_t
  time_us () noexcept
  {
    const std::uint64_t now = counter ();
    if (g_origin == 0)
      {
        g_origin = now;
      }
    const std::uint64_t ticks = now - g_origin;
    const std::uint64_t hz = frequency ();
    return (ticks / hz) * 1000000u + ((ticks % hz) * 1000000u) / hz;
  }

  std::uint32_t
  millis () noexcept
  {
    return static_cast<std::uint32_t> (time_us () / 1000u);
  }

  void
  delay_us (std::uint32_t us) noexcept
  {
    if (us >= 1000u && os::rtos::scheduler::started ()
        && !os::rtos::interrupts::in_handler_mode ())
      {
        os::rtos::sysclock.sleep_for ((us + 999u) / 1000u);
        return;
      }
    const std::uint64_t until = time_us () + us;
    while (time_us () < until)
      {
        asm volatile ("yield");
      }
  }

  void
  dump_log () noexcept
  {
#if defined(SEMIHOST) && defined(HW_BUILD)
    // SYS_WRITE0 takes a NUL-terminated string: hand it over in pieces.
    static char piece[1024 + 1];
    while (g_log_dumped < g_log_size)
      {
        std::size_t n = g_log_size - g_log_dumped;
        if (n > sizeof (piece) - 1)
          {
            n = sizeof (piece) - 1;
          }
        std::memcpy (piece, g_log + g_log_dumped, n);
        piece[n] = '\0';
        semihosting::write_str (piece);
        g_log_dumped += n;
      }
#endif
  }

  namespace
  {
    void
    remember (const char* text, std::size_t n) noexcept
    {
      if (n > sizeof (g_log) - g_log_size)
        {
          n = sizeof (g_log) - g_log_size;
        }
      std::memcpy (g_log + g_log_size, text, n);
      g_log_size += n;
    }
  } // namespace
} // namespace rpiz2w::platform

extern "C" void
kprintf (const char* format, ...)
{
  // One writer at a time: the net thread and the SD thread both report.
  static os::rtos::mutex console{ "console" };
  char text[256];
  std::va_list args;
  va_start (args, format);
  int n = std::vsnprintf (text, sizeof (text), format, args);
  va_end (args);
  if (n < 0)
    {
      return;
    }
  if (n > static_cast<int> (sizeof (text)) - 1)
    {
      n = static_cast<int> (sizeof (text)) - 1;
    }
  const bool lockable = os::rtos::scheduler::started ()
                        && !os::rtos::interrupts::in_handler_mode ();
  if (lockable)
    {
      console.lock ();
    }
  rpiz2w::platform::remember (text, static_cast<std::size_t> (n));
  uart::uart1.puts (text);
  if (lockable)
    {
      console.unlock ();
    }
}

extern "C" unsigned int
net_random (void)
{
  // xorshift64*, seeded from the timer: DHCP transaction ids, DNS ids and
  // TCP initial sequence numbers.
  static std::uint64_t state = 0;
  if (state == 0)
    {
      const std::uint64_t now = timer_arm::get_count ();
      state = now * 0x9e3779b97f4a7c15ull + 0x2545f4914f6cdd1dull;
    }
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return static_cast<unsigned int> ((state * 0x2545f4914f6cdd1dull) >> 32);
}
