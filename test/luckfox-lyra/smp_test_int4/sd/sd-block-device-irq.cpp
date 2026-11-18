/**
 * @file sd/sd-block-device-irq.cpp
 * @brief The card of smp_test_int4 as a µOS++ POSIX I/O block device.
 *
 * The test's own interrupt-driven SD driver (sd/sdmmc.cpp, whose writes
 * complete on the DW-MMC DTO interrupt once the test arms it) under the
 * kernel's posix-io block device, /dev/sd. The test's storage start-up
 * (board-tests/portable-test-storage.cpp, uos_test_storage_start()) mounts it
 * at /sd/ with Chan FatFs, so f_write -> disk_write -> SdMmc::write_blocks of
 * the old glue is now posix write -> Chan FatFs -> this -> SdMmc::write_blocks.
 *
 * The SdMmc instance stays reachable from the application (sd_disk ()), so
 * the test can arm the interrupt on the exact instance the file system writes
 * through. Compiled for this test only (board_test_sd_posix OWN): the board's
 * /dev/sd is the same controller under the polled driver.
 */

#include <cmsis-plus/posix-io/block-device.h>

#include <uos-test-storage.h>

#include "sdmmc.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdint>

namespace
{
  sdcard::SdMmc g_sd_disk; ///< the one instance the file system drives
  bool g_inited = false;

  class irq_block_impl : public os::posix::block_device_impl
  {
  public:
    irq_block_impl (void)
    {
      block_logical_size_bytes_ = 512;
      block_physical_size_bytes_ = 512;
    }

  protected:
    int
    do_vopen (const char*, int, std::va_list) override
    {
      // Polled bring-up at the first open (the mount), then the 24 MHz
      // transfer clock, as the test always ran it.
      if (!g_inited)
        {
          g_inited = g_sd_disk.init ();
          if (g_inited)
            {
              g_sd_disk.set_transfer_clock (24000000u);
            }
        }
      if (!g_inited)
        {
          errno = EIO;
          return -1;
        }
      num_blocks_ = static_cast<blknum_t> (g_sd_disk.capacity_bytes () / 512u);
      return 0;
    }

    ssize_t
    do_read_block (void* buf, blknum_t blknum, std::size_t nblocks) override
    {
      if (!g_sd_disk.read_blocks (static_cast<std::uint32_t> (blknum),
                                  buf,
                                  static_cast<std::uint32_t> (nblocks)))
        {
          errno = EIO;
          return -1;
        }
      return static_cast<ssize_t> (nblocks);
    }

    ssize_t
    do_write_block (const void* buf, blknum_t blknum,
                    std::size_t nblocks) override
    {
      if (!g_sd_disk.write_blocks (static_cast<std::uint32_t> (blknum),
                                   buf,
                                   static_cast<std::uint32_t> (nblocks)))
        {
          errno = EIO;
          return -1;
        }
      return static_cast<ssize_t> (nblocks);
    }

    int
    do_vioctl (int, std::va_list) override
    {
      errno = ENOTTY;
      return -1;
    }

    void
    do_sync (void) override
    {
    }

    int
    do_close (void) override
    {
      return 0;
    }
  };

  os::posix::block_device_implementable<irq_block_impl> g_card{ "sd" };
} // namespace

os::posix::block_device&
uos_test_storage_card (void)
{
  return g_card;
}

sdcard::SdMmc&
sd_disk ()
{
  return g_sd_disk;
}
