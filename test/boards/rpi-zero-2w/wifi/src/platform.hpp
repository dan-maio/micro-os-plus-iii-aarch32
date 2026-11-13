// platform.hpp - time, delays and the console for the Wi-Fi stack.
//
// A monotonic clock from the ARM generic timer, delays that sleep through the
// RTOS when they are long enough to matter, and a printf that keeps a line
// whole when two threads print. Everything printed is also kept in RAM, so a
// run can hand its whole console to the runner in one semihosting call.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rpiz2w::platform
{
  /// Microseconds since the first call (ARM generic timer, CNTPCT via CP15).
  std::uint64_t time_us () noexcept;

  /// Milliseconds since the first call.
  std::uint32_t millis () noexcept;

  /// Wait. One millisecond or more sleeps on the system clock, so other
  /// threads run; shorter waits spin, they are hardware timing constraints.
  void delay_us (std::uint32_t us) noexcept;

  /// Send everything kprintf() has printed so far to the host through
  /// semihosting, in a few large pieces (one trap each). The UART console is
  /// the user's; this is how the runner's log gets the same text. A no-op
  /// without semihosting.
  void dump_log () noexcept;
} // namespace rpiz2w::platform

extern "C"
{
  /// printf on the console UART; one call is one uninterrupted piece of text.
  void kprintf (const char* format, ...) __attribute__ ((format (printf, 1, 2)));
  unsigned int net_random (void);
}
