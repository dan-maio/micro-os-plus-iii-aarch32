/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem module  R0.15a                              /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2024, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:

/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/
/----------------------------------------------------------------------------*/


#ifndef FF_DEFINED
#define FF_DEFINED	5380	/* Revision ID */

#include <cstdint>
#include <cstddef>
#include <utility>
#include <cstring>
#include <iterator>

namespace fatfs {

#if !defined(FFCONF_DEF)
#include "ffconf.h"		/* FatFs configuration options */
#endif
#if FF_DEFINED != FFCONF_DEF
#error Wrong configuration file (ffconf.h).
#endif


/* Integer types used for FatFs API */

#if defined(_WIN32)		/* Windows VC++ (for development only) */
#define FF_INTDEF 2
#include <windows.h>
using QWORD = unsigned __int64;
#include <float.h>
#define isnan(v) _isnan(v)
#define isinf(v) (!_finite(v))

#elif (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || defined(__cplusplus)	/* C99 or later */
#define FF_INTDEF 2
#include <stdint.h>
using UINT = unsigned int;	/* int must be 16-bit or 32-bit */
using BYTE = std::uint8_t;	/* char must be 8-bit */
using WORD = std::uint16_t;	/* 16-bit unsigned */
using DWORD = std::uint32_t;	/* 32-bit unsigned */
using QWORD = std::uint64_t;	/* 64-bit unsigned */
using WCHAR = WORD;	/* UTF-16 code unit */

#else  	/* Earlier than C99 */
#define FF_INTDEF 1
using UINT = unsigned int;	/* int must be 16-bit or 32-bit */
using BYTE = std::uint8_t;	/* char must be 8-bit */
using WORD = unsigned short;	/* short must be 16-bit */
using DWORD = unsigned long;	/* long must be 32-bit */
using WCHAR = WORD;	/* UTF-16 code unit */
#endif


/* Type of file size and LBA variables */

#if FF_FS_EXFAT
#if FF_INTDEF != 2
#error exFAT feature wants C99 or later
#endif
using FSIZE_t = QWORD;
#if FF_LBA64
using LBA_t = QWORD;
#else
using LBA_t = DWORD;
#endif
#else
#if FF_LBA64
#error exFAT needs to be enabled when enable 64-bit LBA
#endif
using FSIZE_t = DWORD;
using LBA_t = DWORD;
#endif



/* Type of path name strings on FatFs API (TCHAR) */

#if FF_USE_LFN && FF_LFN_UNICODE == 1 	/* Unicode in UTF-16 encoding */
using TCHAR = WCHAR;
#define _T(x) L ## x
#define _TEXT(x) L ## x
#elif FF_USE_LFN && FF_LFN_UNICODE == 2	/* Unicode in UTF-8 encoding */
using TCHAR = char;
#define _T(x) u8 ## x
#define _TEXT(x) u8 ## x
#elif FF_USE_LFN && FF_LFN_UNICODE == 3	/* Unicode in UTF-32 encoding */
using TCHAR = DWORD;
#define _T(x) U ## x
#define _TEXT(x) U ## x
#elif FF_USE_LFN && (FF_LFN_UNICODE < 0 || FF_LFN_UNICODE > 3)
#error Wrong FF_LFN_UNICODE setting
#else									/* ANSI/OEM code in SBCS/DBCS */
using TCHAR = char;
#define _T(x) x
#define _TEXT(x) x
#endif



/* Definitions of volume management */

#if FF_MULTI_PARTITION		/* Multiple partition configuration */
typedef struct {
	BYTE pd;	/* Associated physical drive */
	BYTE pt;	/* Associated partition (0:Auto detect, 1-4:Forced partition) */
} PARTITION;
extern PARTITION VolToPart[];	/* Volume to partition mapping table */
#endif

#if FF_STR_VOLUME_ID
#ifndef FF_VOLUME_STRS
extern const char* VolumeStr[FF_VOLUMES];	/* User defined volume ID table */
#endif
#endif



/* Filesystem object structure (FATFS) */

typedef struct {
	BYTE	fs_type;		/* Filesystem type (0:blank filesystem object) */
	BYTE	pdrv;			/* Volume hosting physical drive */
	BYTE	ldrv;			/* Logical drive number (used only when FF_FS_REENTRANT) */
	BYTE	n_fats;			/* Number of FATs (1 or 2) */
	BYTE	wflag;			/* win[] status (1:dirty) */
	BYTE	fsi_flag;		/* Allocation information control (b7:disabled, b0:dirty) */
	WORD	id;				/* Volume mount ID */
	WORD	n_rootdir;		/* Number of root directory entries (FAT12/16) */
	WORD	csize;			/* Cluster size [sectors] */
#if FF_MAX_SS != FF_MIN_SS
	WORD	ssize;			/* Sector size (512, 1024, 2048 or 4096) */
#endif
#if FF_USE_LFN
	WCHAR*	lfnbuf;			/* LFN working buffer */
#endif
#if FF_FS_EXFAT
	BYTE*	dirbuf;			/* Directory entry block scratch pad buffer for exFAT */
#endif
#if !FF_FS_READONLY
	DWORD	last_clst;		/* Last allocated cluster (Unknown if >= n_fatent) */
	DWORD	free_clst;		/* Number of free clusters (Unknown if >= n_fatent-2) */
#endif
#if FF_FS_RPATH
	DWORD	cdir;			/* Current directory start cluster (0:root) */
#if FF_FS_EXFAT
	DWORD	cdc_scl;		/* Containing directory start cluster (invalid when cdir is 0) */
	DWORD	cdc_size;		/* b31-b8:Size of containing directory, b7-b0: Chain status */
	DWORD	cdc_ofs;		/* Offset in the containing directory (invalid when cdir is 0) */
#endif
#endif
	DWORD	n_fatent;		/* Number of FAT entries (number of clusters + 2) */
	DWORD	fsize;			/* Number of sectors per FAT */
	LBA_t	volbase;		/* Volume base sector */
	LBA_t	fatbase;		/* FAT base sector */
	LBA_t	dirbase;		/* Root directory base sector (FAT12/16) or cluster (FAT32/exFAT) */
	LBA_t	database;		/* Data base sector */
#if FF_FS_EXFAT
	LBA_t	bitbase;		/* Allocation bitmap base sector */
#endif
	LBA_t	winsect;		/* Current sector appearing in the win[] */
	BYTE	win[FF_MAX_SS];	/* Disk access window for Directory, FAT (and file data at tiny cfg) */
} FATFS;



/* Object ID and allocation information (FFOBJID) */

typedef struct {
	FATFS*	fs;				/* Pointer to the hosting volume of this object */
	WORD	id;				/* Hosting volume's mount ID */
	BYTE	attr;			/* Object attribute */
	BYTE	stat;			/* Object chain status (b1-0: =0:not contiguous, =2:contiguous, =3:fragmented in this session, b2:sub-directory stretched) */
	DWORD	sclust;			/* Object data start cluster (0:no cluster or root directory) */
	FSIZE_t	objsize;		/* Object size (valid when sclust != 0) */
#if FF_FS_EXFAT
	DWORD	n_cont;			/* Size of first fragment - 1 (valid when stat == 3) */
	DWORD	n_frag;			/* Size of last fragment needs to be written to FAT (valid when not zero) */
	DWORD	c_scl;			/* Containing directory start cluster (valid when sclust != 0) */
	DWORD	c_size;			/* b31-b8:Size of containing directory, b7-b0: Chain status (valid when c_scl != 0) */
	DWORD	c_ofs;			/* Offset in the containing directory (valid when file object and sclust != 0) */
#endif
#if FF_FS_LOCK
	UINT	lockid;			/* File lock ID origin from 1 (index of file semaphore table Files[]) */
#endif
} FFOBJID;



/* File object structure (FIL) */

typedef struct {
	FFOBJID	obj;			/* Object identifier (must be the 1st member to detect invalid object pointer) */
	BYTE	flag;			/* File status flags */
	BYTE	err;			/* Abort flag (error code) */
	FSIZE_t	fptr;			/* File read/write pointer (Zeroed on file open) */
	DWORD	clust;			/* Current cluster of fpter (invalid when fptr is 0) */
	LBA_t	sect;			/* Sector number appearing in buf[] (0:invalid) */
#if !FF_FS_READONLY
	LBA_t	dir_sect;		/* Sector number containing the directory entry (not used at exFAT) */
	BYTE*	dir_ptr;		/* Pointer to the directory entry in the win[] (not used at exFAT) */
#endif
#if FF_USE_FASTSEEK
	DWORD*	cltbl;			/* Pointer to the cluster link map table (nulled on open, set by application) */
#endif
#if !FF_FS_TINY
	BYTE	buf[FF_MAX_SS];	/* File private data read/write window */
#endif
} FIL;



/* Directory object structure (DIR) */

typedef struct {
	FFOBJID	obj;			/* Object identifier */
	DWORD	dptr;			/* Current read/write offset */
	DWORD	clust;			/* Current cluster */
	LBA_t	sect;			/* Current sector (0:Read operation has terminated) */
	BYTE*	dir;			/* Pointer to the directory item in the win[] */
	BYTE	fn[12];			/* SFN (in/out) {body[8],ext[3],status[1]} */
#if FF_USE_LFN
	DWORD	blk_ofs;		/* Offset of current entry block being processed (0xFFFFFFFF:Invalid) */
#endif
#if FF_USE_FIND
	const TCHAR* pat;		/* Pointer to the name matching pattern */
#endif
} DIR;



/* File information structure (FILINFO) */

typedef struct {
	FSIZE_t	fsize;			/* File size */
	WORD	fdate;			/* Modified date */
	WORD	ftime;			/* Modified time */
	BYTE	fattrib;		/* File attribute */
#if FF_USE_LFN
	TCHAR	altname[FF_SFN_BUF + 1];/* Alternative file name */
	TCHAR	fname[FF_LFN_BUF + 1];	/* Primary file name */
#else
	TCHAR	fname[12 + 1];	/* File name */
#endif
} FILINFO;



/* Format parameter structure (MKFS_PARM) */

typedef struct {
	BYTE fmt;			/* Format option (FM_FAT, FM_FAT32, FM_EXFAT and FM_SFD) */
	BYTE n_fat;			/* Number of FATs */
	UINT align;			/* Data area alignment (sector) */
	UINT n_root;		/* Number of root directory entries */
	DWORD au_size;		/* Cluster size (byte) */
} MKFS_PARM;



/* File function return code (FRESULT) */

enum class FRESULT {
	FR_OK = 0,				/* (0) Function succeeded */
	FR_DISK_ERR,			/* (1) A hard error occurred in the low level disk I/O layer */
	FR_INT_ERR,				/* (2) Assertion failed */
	FR_NOT_READY,			/* (3) The physical drive does not work */
	FR_NO_FILE,				/* (4) Could not find the file */
	FR_NO_PATH,				/* (5) Could not find the path */
	FR_INVALID_NAME,		/* (6) The path name format is invalid */
	FR_DENIED,				/* (7) Access denied due to a prohibited access or directory full */
	FR_EXIST,				/* (8) Access denied due to a prohibited access */
	FR_INVALID_OBJECT,		/* (9) The file/directory object is invalid */
	FR_WRITE_PROTECTED,		/* (10) The physical drive is write protected */
	FR_INVALID_DRIVE,		/* (11) The logical drive number is invalid */
	FR_NOT_ENABLED,			/* (12) The volume has no work area */
	FR_NO_FILESYSTEM,		/* (13) Could not find a valid FAT volume */
	FR_MKFS_ABORTED,		/* (14) The f_mkfs function aborted due to some problem */
	FR_TIMEOUT,				/* (15) Could not take control of the volume within defined period */
	FR_LOCKED,				/* (16) The operation is rejected according to the file sharing policy */
	FR_NOT_ENOUGH_CORE,		/* (17) LFN working buffer could not be allocated or given buffer is insufficient in size */
	FR_TOO_MANY_OPEN_FILES,	/* (18) Number of open files > FF_FS_LOCK */
	FR_INVALID_PARAMETER	/* (19) Given parameter is invalid */
};
using enum FRESULT;




/*--------------------------------------------------------------*/
/* FatFs Module Application Interface                           */
/*--------------------------------------------------------------*/

FRESULT f_open (FIL* fp, const TCHAR* path, BYTE mode);				/* Open or create a file */
FRESULT f_close (FIL* fp);											/* Close an open file object */
FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br);			/* Read data from the file */
FRESULT f_write (FIL* fp, const void* buff, UINT btw, UINT* bw);	/* Write data to the file */
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);								/* Move file pointer of the file object */
FRESULT f_truncate (FIL* fp);										/* Truncate the file */
FRESULT f_sync (FIL* fp);											/* Flush cached data of the writing file */
FRESULT f_opendir (DIR* dp, const TCHAR* path);						/* Open a directory */
FRESULT f_closedir (DIR* dp);										/* Close an open directory */
FRESULT f_readdir (DIR* dp, FILINFO* fno);							/* Read a directory item */
FRESULT f_findfirst (DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern);	/* Find first file */
FRESULT f_findnext (DIR* dp, FILINFO* fno);							/* Find next file */
FRESULT f_mkdir (const TCHAR* path);								/* Create a sub directory */
FRESULT f_unlink (const TCHAR* path);								/* Delete an existing file or directory */
FRESULT f_rename (const TCHAR* path_old, const TCHAR* path_new);	/* Rename/Move a file or directory */
FRESULT f_stat (const TCHAR* path, FILINFO* fno);					/* Get file status */
FRESULT f_chmod (const TCHAR* path, BYTE attr, BYTE mask);			/* Change attribute of a file/dir */
FRESULT f_utime (const TCHAR* path, const FILINFO* fno);			/* Change timestamp of a file/dir */
FRESULT f_chdir (const TCHAR* path);								/* Change current directory */
FRESULT f_chdrive (const TCHAR* path);								/* Change current drive */
FRESULT f_getcwd (TCHAR* buff, UINT len);							/* Get current directory */
FRESULT f_getfree (const TCHAR* path, DWORD* nclst, FATFS** fatfs);	/* Get number of free clusters on the drive */
FRESULT f_getlabel (const TCHAR* path, TCHAR* label, DWORD* vsn);	/* Get volume label */
FRESULT f_setlabel (const TCHAR* label);							/* Set volume label */
FRESULT f_forward (FIL* fp, UINT(*func)(const BYTE*,UINT), UINT btf, UINT* bf);	/* Forward data to the stream */
FRESULT f_expand (FIL* fp, FSIZE_t fsz, BYTE opt);					/* Allocate a contiguous block to the file */
FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt);			/* Mount/Unmount a logical drive */
FRESULT f_mkfs (const TCHAR* path, const MKFS_PARM* opt, void* work, UINT len);	/* Create a FAT volume */
FRESULT f_fdisk (BYTE pdrv, const LBA_t ptbl[], void* work);		/* Divide a physical drive into some partitions */
FRESULT f_setcp (WORD cp);											/* Set current code page */
int f_putc (TCHAR c, FIL* fp);										/* Put a character to the file */
int f_puts (const TCHAR* str, FIL* cp);								/* Put a string to the file */
int f_printf (FIL* fp, const TCHAR* str, ...);						/* Put a formatted string to the file */
TCHAR* f_gets (TCHAR* buff, int len, FIL* fp);						/* Get a string from the file */

/* Some API functions are implemented as inline functions */

inline int f_eof(const FIL* fp) noexcept {
    return static_cast<int>(fp->fptr == fp->obj.objsize);
}
inline BYTE f_error(const FIL* fp) noexcept {
    return fp->err;
}
inline FSIZE_t f_tell(const FIL* fp) noexcept {
    return fp->fptr;
}
inline FSIZE_t f_size(const FIL* fp) noexcept {
    return fp->obj.objsize;
}
inline FRESULT f_rewind(FIL* fp) noexcept {
    return f_lseek(fp, 0);
}
inline FRESULT f_rewinddir(DIR* dp) noexcept {
    return f_readdir(dp, nullptr);
}
inline FRESULT f_rmdir(const TCHAR* path) noexcept {
    return f_unlink(path);
}
inline FRESULT f_unmount(const TCHAR* path) noexcept {
    return f_mount(nullptr, path, 0);
}




/*--------------------------------------------------------------*/
/* Additional Functions                                         */
/*--------------------------------------------------------------*/

/* RTC function (provided by user) */
#if !FF_FS_READONLY && !FF_FS_NORTC
DWORD get_fattime (void);	/* Get current time */
#endif


/* LFN support functions (defined in ffunicode.c) */

#if FF_USE_LFN >= 1
WCHAR ff_oem2uni (WCHAR oem, WORD cp);	/* OEM code to Unicode conversion */
WCHAR ff_uni2oem (DWORD uni, WORD cp);	/* Unicode to OEM code conversion */
DWORD ff_wtoupper (DWORD uni);			/* Unicode upper-case conversion */
#endif


/* O/S dependent functions (samples available in ffsystem.c) */

#if FF_USE_LFN == 3		/* Dynamic memory allocation */
void* ff_memalloc (UINT msize);		/* Allocate memory block */
void ff_memfree (void* mblock);		/* Free memory block */
#endif
#if FF_FS_REENTRANT		/* Sync functions */
int ff_mutex_create (int vol);		/* Create a sync object */
void ff_mutex_delete (int vol);		/* Delete a sync object */
int ff_mutex_take (int vol);		/* Lock sync object */
void ff_mutex_give (int vol);		/* Unlock sync object */
#endif




/*--------------------------------------------------------------*/
/* Flags and Offset Address                                     */
/*--------------------------------------------------------------*/

/* File access mode and open method flags (3rd argument of f_open function) */
inline constexpr BYTE FA_READ = 0x01;
inline constexpr BYTE FA_WRITE = 0x02;
inline constexpr BYTE FA_OPEN_EXISTING = 0x00;
inline constexpr BYTE FA_CREATE_NEW = 0x04;
inline constexpr BYTE FA_CREATE_ALWAYS = 0x08;
inline constexpr BYTE FA_OPEN_ALWAYS = 0x10;
inline constexpr BYTE FA_OPEN_APPEND = 0x30;

/* Fast seek controls (2nd argument of f_lseek function) */
inline constexpr FSIZE_t CREATE_LINKMAP = static_cast<FSIZE_t>(-1);

/* Format options (2nd argument of f_mkfs function) */
inline constexpr BYTE FM_FAT = 0x01;
inline constexpr BYTE FM_FAT32 = 0x02;
inline constexpr BYTE FM_EXFAT = 0x04;
inline constexpr BYTE FM_ANY = 0x07;
inline constexpr BYTE FM_SFD = 0x08;

/* Filesystem type (FATFS.fs_type) */
inline constexpr BYTE FS_FAT12 = 1;
inline constexpr BYTE FS_FAT16 = 2;
inline constexpr BYTE FS_FAT32 = 3;
inline constexpr BYTE FS_EXFAT = 4;

/* File attribute bits for directory entry (FILINFO.fattrib) */
inline constexpr BYTE AM_RDO = 0x01;	/* Read only */
inline constexpr BYTE AM_HID = 0x02;	/* Hidden */
inline constexpr BYTE AM_SYS = 0x04;	/* System */
inline constexpr BYTE AM_DIR = 0x10;	/* Directory */
inline constexpr BYTE AM_ARC = 0x20;	/* Archive */


/*--------------------------------------------------------------*/
/* Modern C++23 RAII File/Dir wrappers                           */
/*--------------------------------------------------------------*/

class File {
public:
    File() noexcept = default;

    ~File() {
        close();
    }

    File(File&& other) noexcept : fp_(other.fp_), is_open_(other.is_open_) {
        other.fp_ = FIL{};
        other.is_open_ = false;
    }

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            fp_ = other.fp_;
            is_open_ = other.is_open_;
            other.fp_ = FIL{};
            other.is_open_ = false;
        }
        return *this;
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(const TCHAR* path, BYTE mode) noexcept {
        last_res_ = open(path, mode);
    }

    FRESULT open(const TCHAR* path, BYTE mode) noexcept {
        close();
        FRESULT res = f_open(&fp_, path, mode);
        last_res_ = res;
        if (res == FR_OK) {
            is_open_ = true;
        }
        return res;
    }

    FRESULT last_result() const noexcept { return last_res_; }

    FRESULT close() noexcept {
        if (is_open_) {
            is_open_ = false;
            return f_close(&fp_);
        }
        return FR_OK;
    }

    bool is_open() const noexcept { return is_open_; }
    explicit operator bool() const noexcept { return is_open_; }

    FRESULT read(void* buff, UINT btr, UINT* br) noexcept {
        return f_read(&fp_, buff, btr, br);
    }

    FRESULT write(const void* buff, UINT btw, UINT* bw) noexcept {
        return f_write(&fp_, buff, btw, bw);
    }

    FRESULT seek(FSIZE_t ofs) noexcept {
        return f_lseek(&fp_, ofs);
    }

    FRESULT truncate() noexcept {
        return f_truncate(&fp_);
    }

    FRESULT sync() noexcept {
        return f_sync(&fp_);
    }

    FSIZE_t tell() const noexcept {
        return fp_.fptr;
    }

    FSIZE_t size() const noexcept {
        return fp_.obj.objsize;
    }

    bool eof() const noexcept {
        return fp_.fptr == fp_.obj.objsize;
    }

    BYTE error() const noexcept {
        return fp_.err;
    }

    FRESULT rewind() noexcept {
        return seek(0);
    }

    int putc(TCHAR c) noexcept {
        return f_putc(c, &fp_);
    }

    int puts(const TCHAR* str) noexcept {
        return f_puts(str, &fp_);
    }

    template <typename... Args>
    int printf(const TCHAR* format, Args&&... args) noexcept {
        return f_printf(&fp_, format, std::forward<Args>(args)...);
    }

    TCHAR* gets(TCHAR* buff, int len) noexcept {
        return f_gets(buff, len, &fp_);
    }

    FIL* raw() noexcept { return &fp_; }
    const FIL* raw() const noexcept { return &fp_; }

private:
    FIL fp_{};
    bool is_open_ = false;
    FRESULT last_res_ = FR_OK;
};

class Dir {
public:
    Dir() noexcept = default;

    ~Dir() {
        close();
    }

    Dir(Dir&& other) noexcept : dp_(other.dp_), is_open_(other.is_open_) {
        other.dp_ = DIR{};
        other.is_open_ = false;
    }

    Dir& operator=(Dir&& other) noexcept {
        if (this != &other) {
            close();
            dp_ = other.dp_;
            is_open_ = other.is_open_;
            other.dp_ = DIR{};
            other.is_open_ = false;
        }
        return *this;
    }

    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;

    explicit Dir(const TCHAR* path) noexcept {
        last_res_ = open(path);
    }

    FRESULT open(const TCHAR* path) noexcept {
        close();
        FRESULT res = f_opendir(&dp_, path);
        last_res_ = res;
        if (res == FR_OK) {
            is_open_ = true;
        }
        return res;
    }

    FRESULT last_result() const noexcept { return last_res_; }

    FRESULT close() noexcept {
        if (is_open_) {
            is_open_ = false;
            return f_closedir(&dp_);
        }
        return FR_OK;
    }

    bool is_open() const noexcept { return is_open_; }
    explicit operator bool() const noexcept { return is_open_; }

    FRESULT readdir(FILINFO* fno) noexcept {
        return f_readdir(&dp_, fno);
    }

    FRESULT rewind() noexcept {
        return f_readdir(&dp_, nullptr);
    }

    DIR* raw() noexcept { return &dp_; }
    const DIR* raw() const noexcept { return &dp_; }

    class Iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = FILINFO;
        using pointer = const FILINFO*;
        using reference = const FILINFO&;
        using iterator_category = std::input_iterator_tag;

        Iterator() noexcept : dir_(nullptr) {
            fno_.fname[0] = 0;
        }

        explicit Iterator(Dir* dir) noexcept : dir_(dir) {
            if (dir_) {
                advance();
            }
        }

        reference operator*() const noexcept { return fno_; }
        pointer operator->() const noexcept { return &fno_; }

        Iterator& operator++() noexcept {
            advance();
            return *this;
        }

        void operator++(int) noexcept {
            advance();
        }

        bool operator==(const Iterator& other) const noexcept {
            bool this_end = (dir_ == nullptr || fno_.fname[0] == 0);
            bool other_end = (other.dir_ == nullptr || other.fno_.fname[0] == 0);
            if (this_end && other_end) return true;
            return (dir_ == other.dir_) && (std::strcmp(fno_.fname, other.fno_.fname) == 0);
        }

    private:
        void advance() noexcept {
            if (dir_ && dir_->is_open()) {
                FRESULT res = dir_->readdir(&fno_);
                if (res != FR_OK || fno_.fname[0] == 0) {
                    dir_ = nullptr;
                }
            } else {
                dir_ = nullptr;
            }
        }

        Dir* dir_;
        FILINFO fno_;
    };

    Iterator begin() noexcept {
        return Iterator(this);
    }

    Iterator end() noexcept {
        return Iterator();
    }

private:
    DIR dp_{};
    bool is_open_ = false;
    FRESULT last_res_ = FR_OK;
};

}  // namespace fatfs

namespace fs = fatfs;

#endif /* FF_DEFINED */
