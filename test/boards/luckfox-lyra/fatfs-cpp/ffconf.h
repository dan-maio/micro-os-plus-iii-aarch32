/*---------------------------------------------------------------------------/
/  FatFs configuration for the Luckfox Lyra (RK3506) bare-metal SD driver
/---------------------------------------------------------------------------/
/  Tuned for the AMP StateOS SD-card test:
/   - Read/write, long file names, sub-directories, f_mkfs (format),
/     f_rename (move), string helpers (f_printf/f_puts/f_gets).
/   - Single volume, fixed 512-byte sectors, no RTC (fixed timestamp),
/     no re-entrancy (the test drives it from one task).
/  Keep FFCONF_DEF in lock-step with FF_DEFINED in ff.h (rev 5380 = R0.15a).
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	5380

/*----- Function Configurations -----*/
#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		1
#define FF_USE_MKFS		1	/* f_mkfs(): format the card */
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	1	/* f_getlabel()/f_setlabel() */
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	1	/* f_puts()/f_printf()/f_gets() */
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3

/*----- Locale and Namespace Configurations -----*/
#define FF_CODE_PAGE	437	/* U.S. (small OEM table for LFN) */
#define FF_USE_LFN		2	/* LFN with work area on the caller stack (thread-safe) */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0	/* API uses ANSI/OEM char strings (TCHAR = char) */
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0

/*----- Drive/Volume Configurations -----*/
#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/*----- System Configurations -----*/
#define FF_FS_TINY		0
#define FF_FS_EXFAT		0
#define FF_FS_NORTC		1	/* no RTC: use the fixed date below */
#define FF_NORTC_MON	7
#define FF_NORTC_MDAY	5
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	1
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
