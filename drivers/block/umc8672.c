/*
 *  linux/drivers/block/umc8672.c	Version 0.02  Feb 06, 1996
 *
 *  Copyright (C) 1995-1996  Linus Torvalds & author (see below)
 */

/*
 *  Principal Author/Maintainer:  PODIEN@hml2.atlas.de (Wolfram Podien)
 *
 *  This file provides support for the advanced features
 *  of the UMC 8672 IDE interface.
 *
 *  Version 0.01	Initial version, hacked out of ide.c,
 *			and #include'd rather than compiled separately.
 *			This will get cleaned up in a subsequent release.
 *
 *  Version 0.02	now configs/compiles separate from ide.c  -ml
 */

/*
 * VLB Controller Support from 
 * Wolfram Podien
 * Rohoefe 3
 * D28832 Achim
 * Germany
 *
 * To enable UMC8672 support there must a lilo line like
 * append="hd=umc8672"...
 * To set the speed according to the abilities of the hardware there must be a
 * line like
 * #define UMC_DRIVE0 11
 * in the beginning of the driver, which sets the speed of drive 0 to 11 (there
 * are some lines present). 0 - 11 are allowed speed values. These values are
 * the results from the DOS speed test programm supplied from UMC. 11 is the 
 * highest speed (about PIO mode 3)
 */
#undef REALLY_SLOW_IO		/* most systems can safely undef this */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <asm/io.h>
#include "ide.h"

/*
 * The speeds will eventually become selectable using hdparm via ioctl's,
 * but for now they are coded here:
 */
#define UMC_DRIVE0      1              /* DOS measured drive speeds */
#define UMC_DRIVE1      1              /* 0 to 11 allowed */
#define UMC_DRIVE2      1              /* 11 = Fastest Speed */
#define UMC_DRIVE3      1              /* In case of crash reduce speed */

static byte current_speeds[4] = {UMC_DRIVE0, UMC_DRIVE1, UMC_DRIVE2, UMC_DRIVE3};
static const byte pio_to_umc [5] = {0,3,6,10,11};	/* rough guesses */

/*       0    1    2    3    4    5    6    7    8    9    10   11      */
static const byte speedtab [3][12] = {
	{0xf, 0xb, 0x2, 0x2, 0x2, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1 },
	{0x3, 0x2, 0x2, 0x2, 0x2, 0x2, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1 },
	{0xff,0xcb,0xc0,0x58,0x36,0x33,0x23,0x22,0x21,0x11,0x10,0x0}};

static void out_umc (char port,char wert)
{
	outb_p (port,0x108);
	outb_p (wert,0x109);
}

static byte in_umc (char port)
{
	outb_p (port,0x108);
	return inb_p (0x109);
}

static void umc_set_speeds (byte speeds[])
{
	int i, tmp;
	unsigned long flags;

	save_flags(flags);
	cli ();
	outb_p (0x5A,0x108); /* enable umc */

	out_umc (0xd7,(speedtab[0][speeds[2]] | (speedtab[0][speeds[3]]<<4)));
	out_umc (0xd6,(speedtab[0][speeds[0]] | (speedtab[0][speeds[1]]<<4)));
	tmp = 0;
	for (i = 3; i >= 0; i--)
	{
		tmp = (tmp << 2) | speedtab[1][speeds[i]];
	}
	out_umc (0xdc,tmp);
	for (i = 0;i < 4; i++)
	{
		out_umc (0xd0+i,speedtab[2][speeds[i]]);
		out_umc (0xd8+i,speedtab[2][speeds[i]]);
	}
	outb_p (0xa5,0x108); /* disable umc */
	restore_flags(flags);

	printk ("umc8672: drive speeds [0 to 11]: %d %d %d %d\n",
		speeds[0], speeds[1], speeds[2], speeds[4]);
}

static void tune_umc (ide_drive_t *drive, byte pio)
{
	if (pio == 255)  {	/* auto-tune */
		struct hd_driveid *id = drive->id;
		pio = id->tPIO;
		if (id->field_valid & 0x02) {
			if (id->eide_pio_modes & 0x01)
				pio = 3;
			if (id->eide_pio_modes & 0x02)
				pio = 4;
		}
	}
	if (pio > 4)
		pio = 4;
	current_speeds[drive->name[2] - 'a'] = pio_to_umc[pio];
	umc_set_speeds (current_speeds);
}

void init_umc8672 (void)	/* called from ide.c */
{
	unsigned long flags;

	if (check_region(0x108, 2)) {
		printk("\numc8672: PORTS 0x108-0x109 ALREADY IN USE\n");
		return;
	}
	save_flags(flags);
	cli ();
	outb_p (0x5A,0x108); /* enable umc */
	if (in_umc (0xd5) != 0xa0)
	{
		sti ();
		printk ("umc8672: not found\n");
		return;  
	}
	outb_p (0xa5,0x108); /* disable umc */
	restore_flags(flags);

	umc_set_speeds (current_speeds);

	request_region(0x108, 2, "umc8672");
	ide_hwifs[0].chipset = ide_umc8672;
	ide_hwifs[1].chipset = ide_umc8672;
	ide_hwifs[0].tuneproc = &tune_umc;
	ide_hwifs[1].tuneproc = &tune_umc;

}