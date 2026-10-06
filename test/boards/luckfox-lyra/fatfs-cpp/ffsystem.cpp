/**
 * @file ffsystem.cpp
 * @brief FatFs R0.15a OS-dependent layer for StateOS (stateos_full).
 *
 * Replaces the stock multi-OS sample with a StateOS implementation of the
 * R0.15a sync API (ff_mutex_create/delete/take/give). One priority-inherit
 * mutex per volume (+1 system mutex, index FF_VOLUMES) makes the FatFs API
 * safe to call from any number of concurrent tasks (FF_FS_REENTRANT=1).
 *
 * The mutexes are statically constructed StateOS objects (no heap), so
 * create/delete are trivial. ff_mutex_take honours FF_FS_TIMEOUT (in
 * kernel ticks == ms at the 1 ms tick).
 */

#include "ff.hpp"

#if FF_USE_LFN == 3     /* dynamic LFN buffers */
#include <cstdlib>

namespace fatfs {
void* ff_memalloc(UINT msize) { return std::malloc(static_cast<size_t>(msize)); }
void  ff_memfree(void* mblock) { std::free(mblock); }
} // namespace fatfs
#endif

#if FF_FS_REENTRANT

#include "osal.hpp"

namespace fatfs {

static osal::Mutex ff_mtx[FF_VOLUMES + 1];   /* per volume + system */

int ff_mutex_create(int vol)     /* 1: succeeded */
{
    (void)vol;                   /* statically constructed — nothing to do */
    return 1;
}

void ff_mutex_delete(int vol)
{
    (void)vol;
}

int ff_mutex_take(int vol)       /* 1: got the grant, 0: timeout */
{
    return ff_mtx[vol].waitFor(FF_FS_TIMEOUT) == E_SUCCESS ? 1 : 0;
}

void ff_mutex_give(int vol)
{
    ff_mtx[vol].give();
}

} // namespace fatfs

#endif /* FF_FS_REENTRANT */
