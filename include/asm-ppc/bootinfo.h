/*
 * Non-machine dependent bootinfo structure.  Basic idea
 * borrowed from the m68k.
 *
 * Copyright (C) 1999 Cort Dougan <cort@ppc.kernel.org>
 */


#ifndef _PPC_BOOTINFO_H
#define _PPC_BOOTINFO_H

struct bi_record {
    unsigned short tag;			/* tag ID */
    unsigned long size;			/* size of record (in bytes) */
    unsigned long data[0];		/* data */
};

#define BI_FIRST		0x1010  /* first record - marker */
#define BI_LAST			0x1011	/* last record - marker */
#define BI_CMD_LINE		0x1012
#define BI_BOOTLOADER_ID	0x1013
#define BI_INITRD		0x1014
#define BI_SYSMAP		0x1015
#define BI_MACHTYPE		0x1016

#endif /* _PPC_BOOTINFO_H */

