/*
 * mailbox.cpp — VideoCore property-channel client for BCM2837.
 *
 * Two things must be right or the exchange fails silently:
 *
 *   1. The outbound status is the SINGLE register at 0x18 (bit31 FULL, bit30
 *      EMPTY). There is no separate 0x38 status: reading it returns an
 *      unmapped value that looks permanently FULL, which is exactly how the
 *      first hardware run failed.
 *   2. The VideoCore is normally told a BUS address, not an ARM physical one:
 *      the ARM physical address ORed with 0xC0000000 selects the VideoCore's
 *      uncached alias, and the low four bits carry the channel number. Some
 *      firmware accepts the plain ARM physical address, so a failed aliased
 *      call is retried without it before giving up.
 *
 * The buffer is written through a Normal write-back cacheable mapping (see
 * mmu.cpp), so the lines are cleaned/invalidated to the point of coherency
 * either side of the doorbell. This is the ONLY place in the USB work that
 * needs cache maintenance: the USB data path is PIO.
 *
 * One file serves both ARMv7-A and ARMv8-A. The pointer arithmetic uses
 * uintptr_t, correct on either, and the cache-maintenance instruction is
 * selected inline -- the same convention usb_dwc2.cpp uses. That instruction
 * is the only ISA difference in this file.
 */
#include <cstdint>
#include <atomic>

#include "mailbox.hpp"
#include "bcm2837.hpp"

namespace mailbox
{

namespace
{

// ARM <-> VideoCore legacy mailbox (property channel = 8).
constexpr std::uint32_t kMboxBase   = bcm::PERIPH_BASE + 0xB880u;
constexpr std::uint32_t kMboxRead   = kMboxBase + 0x00u;
constexpr std::uint32_t kMboxStatus = kMboxBase + 0x18u;
constexpr std::uint32_t kMboxWrite  = kMboxBase + 0x20u;

constexpr std::uint32_t kStatusFull  = 0x8000'0000u;
constexpr std::uint32_t kStatusEmpty = 0x4000'0000u;

constexpr std::uint32_t kChannelProperty = 8u;

// ARM physical -> VideoCore bus address (uncached alias).
constexpr std::uint32_t kBusAlias = 0xC000'0000u;

// Bounded spin so a wedged VideoCore cannot hang the core forever.
constexpr std::uint32_t kSpinLimit = 4'000'000u;

std::uint32_t g_last_code  = 0u;
std::uint32_t g_last_state = 0u;
bool          g_last_alias = true;

/// Clean AND invalidate the data cache over [addr, addr+len) to the point of
/// coherency, 64-byte lines (Cortex-A7 and Cortex-A53 both use 64).
void
cache_flush (const void* addr, std::uint32_t len) noexcept
{
  constexpr std::uintptr_t kLine = 64u;
  std::uintptr_t p = reinterpret_cast<std::uintptr_t> (addr) & ~(kLine - 1u);
  const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (addr) + len;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; p < end; p += kLine)
    {
#if defined(__aarch64__)
      // Clean and invalidate by VA to PoC.
      __asm__ volatile ("dc civac, %0" :: "r" (p) : "memory");
#else
      // DCCIMVAC: clean and invalidate by MVA to PoC.
      __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1" :: "r" (p) : "memory");
#endif
    }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static std::atomic_flag s_mbox_lock = ATOMIC_FLAG_INIT;

bool
do_call (std::uint32_t* buf, bool use_alias) noexcept
{
  const std::uint32_t size = buf[0];
  const std::uint32_t addr = static_cast<std::uint32_t> (
      reinterpret_cast<std::uintptr_t> (buf));
  if ((addr & 0xFu) != 0u)
    {
      return false; // caller broke the 16-byte alignment contract
    }

  while (s_mbox_lock.test_and_set (std::memory_order_acquire))
    {
#if defined(__arm__) || defined(__aarch64__)
      __asm__ volatile ("yield" ::: "memory");
#endif
    }

  cache_flush (buf, size);

  const std::uint32_t base = use_alias ? (addr | kBusAlias) : addr;
  const std::uint32_t msg = (base & ~0xFu) | kChannelProperty;

  // Wait for room in the outbound mailbox, then ring. One status register
  // (0x18) reports both directions.
  for (std::uint32_t i = 0;
       (bcm::mmio_read (kMboxStatus) & kStatusFull) != 0u; ++i)
    {
      if (i >= kSpinLimit)
        {
          s_mbox_lock.clear (std::memory_order_release);
          return false;
        }
    }
  bcm::dsb ();
  bcm::mmio_write (kMboxWrite, msg);

  // Wait for OUR reply: the mailbox is shared, so replies for other channels
  // must be drained and ignored rather than mistaken for this one.
  for (;;)
    {
      for (std::uint32_t i = 0;
           (bcm::mmio_read (kMboxStatus) & kStatusEmpty) != 0u; ++i)
        {
          if (i >= kSpinLimit)
            {
              s_mbox_lock.clear (std::memory_order_release);
              return false;
            }
        }
      bcm::dsb ();
      const std::uint32_t got = bcm::mmio_read (kMboxRead);
      if ((got & 0xFu) == kChannelProperty)
        {
          break;
        }
    }

  cache_flush (buf, size);

  const bool ok = (buf[1] == kRespSuccess);
  s_mbox_lock.clear (std::memory_order_release);
  return ok;
}

void
build_power_request (std::uint32_t* buf, std::uint32_t device_id,
                     std::uint32_t state) noexcept
{
  buf[0] = 8u * 4u; // total size in bytes
  buf[1] = 0u;      // request
  buf[2] = kTagSetPowerState;
  buf[3] = 8u;      // value buffer bytes
  buf[4] = 8u;      // value length bytes
  buf[5] = device_id;
  buf[6] = state;
  buf[7] = 0u;      // end tag
}

} // anonymous namespace

bool
property_call (std::uint32_t* buf) noexcept
{
  return do_call (buf, true);
}

bool
set_power_state (std::uint32_t device_id, bool on) noexcept
{
  // 16-byte aligned, and sized for one tag with an 8-byte value buffer.
  alignas (16) std::uint32_t buf[8];

  // state bit0 = on/off, bit1 = wait for the domain to settle before replying.
  const std::uint32_t state = on ? 0x3u : 0x2u;

  build_power_request (buf, device_id, state);
  bool ok = do_call (buf, true);
  g_last_alias = true;
  if (!ok)
    {
      // Rebuild (a partial reply may have touched the buffer) and retry with
      // the plain ARM physical address some firmware prefers.
      build_power_request (buf, device_id, state);
      ok = do_call (buf, false);
      g_last_alias = false;
    }

  g_last_code = buf[1];
  g_last_state = buf[6];

  if (!ok)
    {
      return false;
    }
  // Accepting the tag is not the same as the domain actually being on.
  return ((buf[6] & 0x1u) != 0u) == on;
}

std::uint32_t
last_code () noexcept
{
  return g_last_code;
}

std::uint32_t
last_state () noexcept
{
  return g_last_state;
}

bool
last_used_alias () noexcept
{
  return g_last_alias;
}

} // namespace mailbox
