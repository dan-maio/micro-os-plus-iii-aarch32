/**
 * @file sd/diskio_sd_irq.cpp
 * @brief FatFS disk I/O glue for smp_test_int4 — same bridge as the shared
 *        src/sdcard/fatfs_cpp/diskio_sdmmc.cpp, but the SdMmc instance is
 *        reachable from the application (a plain namespace global + accessor)
 *        so the test can drive bring-up, clock, and interrupt arming on the
 *        exact instance FatFS writes through. Compiled in place of the shared
 *        diskio for this test; shared sources stay untouched.
 */

#include "ff.hpp"
#include "diskio.hpp"
#include "sdmmc.hpp"

namespace fatfs {

sdcard::SdMmc g_sd_disk;                 ///< the one instance FatFS drives
namespace { bool g_inited = false; }

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return g_inited ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!g_inited) g_inited = g_sd_disk.init();
    return g_inited ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited) return RES_NOTRDY;
    return g_sd_disk.read_blocks(static_cast<std::uint32_t>(sector), buff, count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited) return RES_NOTRDY;
    return g_sd_disk.write_blocks(static_cast<std::uint32_t>(sector), buff, count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited && cmd != CTRL_SYNC) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) =
                               static_cast<LBA_t>(g_sd_disk.capacity_bytes() / 512U);
                           return RES_OK;
    case GET_SECTOR_SIZE:  *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *static_cast<DWORD*>(buff) = 1;  return RES_OK;
    default:               return RES_PARERR;
    }
}

}  // namespace fatfs

/// Application accessor: the SdMmc instance FatFS reads/writes through.
sdcard::SdMmc& sd_disk() { return fatfs::g_sd_disk; }
