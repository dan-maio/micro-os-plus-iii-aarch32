/**
 * @file sdcard/fatfs_cpp/diskio_sdmmc.cpp
 * @brief FatFs (C++23 port) low-level disk I/O glue for the RK3506 SD driver.
 *
 * Same role as sdcard/fatfs/diskio_sdmmc.cpp, but targets the C++23 port:
 * the disk_* functions live in `namespace fatfs` and return the port's
 * `enum class DRESULT` / `DSTATUS`. Bridges to the C++ `sdcard::SdMmc`.
 */

#include "ff.hpp"
#include "diskio.hpp"
#include <sdmmc.hpp>   // devices/soc/rk3506 -- the predecessor had it at ../sdmmc.hpp

namespace fatfs {
namespace {
sdcard::SdMmc g_disk;
bool          g_inited = false;
}  // namespace

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return g_inited ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!g_inited) g_inited = g_disk.init();
    return g_inited ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited) return RES_NOTRDY;
    return g_disk.read_blocks(static_cast<std::uint32_t>(sector), buff, count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited) return RES_NOTRDY;
    return g_disk.write_blocks(static_cast<std::uint32_t>(sector), buff, count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_inited && cmd != CTRL_SYNC) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) =
                               static_cast<LBA_t>(g_disk.capacity_bytes() / 512U);
                           return RES_OK;
    case GET_SECTOR_SIZE:  *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *static_cast<DWORD*>(buff) = 1;  return RES_OK;
    default:               return RES_PARERR;
    }
}

}  // namespace fatfs
