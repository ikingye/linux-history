/*
 *  linux/drivers/char/pcxx.c
 * 
 *  Written by Troy De Jongh, November, 1994
 *
 *  Copyright (C) 1994,1995 Troy De Jongh
 *  This software may be used and distributed according to the terms 
 *  of the GNU General Public License.
 *
 *  This driver is for the DigiBoard PC/Xe and PC/Xi line of products.
 *
 *  This driver does NOT support DigiBoard's fastcook FEP option and
 *  does not support the transparent print (i.e. digiprint) option.
 *
 * This Driver is currently maintained by Christoph Lameter (christoph@lameter.com)
 *
 * Please contact digi for support issues at digilnux@dgii.com.
 * Some more information can be found at
 * http://lameter.com/digi.
 *
 *  1.5.2 Fall 1995 Bug fixes by David Nugent
 *  1.5.3 March 9, 1996 Christoph Lameter: Fixed 115.2K Support. Memory
 *		allocation harmonized with 1.3.X Series.
 *  1.5.4 March 30, 1996 Christoph Lameter: Fixup for 1.3.81. Use init_bh
 *		instead of direct assignment to kernel arrays.
 *  1.5.5 April 5, 1996 Major device numbers corrected.
 *              Mike McLagan<mike.mclagan@linux.org>: Add setup
 *              variable handling, instead of using the old pcxxconfig.h
 *  1.5.6 April 16, 1996 Christoph Lameter: Pointer cleanup, macro cleanup.
 *		Call out devices changed to /dev/cudxx.
 *  1.5.7 July 22, 1996 Martin Mares: CLOCAL fix, pcxe_table clearing.
 *		David Nugent: Bug in pcxe_open.
 *		Brian J. Murrell: Modem Control fixes, Majors correctly assigned
 *  1.6.1 April 6, 1997 Bernhard Kaindl: fixed virtual memory access for 2.1
 *              i386-kernels and use on other archtitectures, Allowing use
 *              as module, added module parameters, added switch to enable
 *              verbose messages to assist user during card configuration.
 *              Currently only tested on a PC/Xi card, but should work on Xe
 *              and Xeve also.
 *  1.6.2 August, 7, 2000: Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *  		get rid of panics, release previously allocated resources
 *  1.6.3 August, 23, 2000: Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *  		cleaned up wrt verify_area.
 *              Christoph Lameter: Update documentation, email addresses
 *              and URLs. Remove some obsolete code.
 *
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/delay.h>
#include <linux/serial.h>
#include <linux/tty_driver.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>

#ifndef MODULE
#include <linux/ctype.h> /* We only need it for parsing the "digi="-line */
#endif

#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>

#define VERSION 	"1.6.3"

#include "digi.h"
#include "fep.h"
#include "pcxx.h"
#include "digi_fep.h"
#include "digi_bios.h"

/*
 * Define one default setting if no digi= config line is used.
 * Default is altpin = disabled, 16 ports, I/O 200h, Memory 0D0000h
 */
static struct board_info boards[MAX_DIGI_BOARDS] = { {
/* Board is enabled       */	ENABLED,
/* Type is auto-detected  */	0,
/* altping is disabled    */    DISABLED,
/* number of ports = 16   */	16,
/* io address is 0x200    */	0x200,
/* card memory at 0xd0000 */	0xd0000,
/* first minor device no. */	0
} };
 
static int verbose = 0;
static int debug   = 0;

#ifdef MODULE
/* Variables for insmod */
static int io[]           = {0, 0, 0, 0};
static int membase[]      = {0, 0, 0, 0};
static int memsize[]      = {0, 0, 0, 0};
static int altpin[]       = {0, 0, 0, 0};
static int numports[]     = {0, 0, 0, 0};

MODULE_AUTHOR("Bernhard Kaindl");
MODULE_DESCRIPTION("Digiboard PC/X{i,e,eve} driver");
MODULE_LICENSE("GPL");
MODULE_PARM(verbose,     "i");
MODULE_PARM(debug,       "i");
MODULE_PARM(io,          "1-4i");
MODULE_PARM(membase,     "1-4i");
MODULE_PARM(memsize,     "1-4i");
MODULE_PARM(altpin,      "1-4i");
MODULE_PARM(numports,    "1-4i");

#endif /* MODULE */

static int numcards = 1;
static int nbdevs = 0;
 
static struct channel    *digi_channels;
 
int pcxx_ncook=sizeof(pcxx_cook);
int pcxx_nbios=sizeof(pcxx_bios);

#define pcxxassert(x, msg)  if(!(x)) pcxx_error(__LINE__, msg)

#define FEPTIMEOUT 200000  
#define SERIAL_TYPE_NORMAL	1
#define PCXE_EVENT_HANGUP   1

static struct tty_driver *pcxe_driver;

static struct timer_list pcxx_timer;

static void pcxxpoll(unsigned long dummy);
static void fepcmd(struct channel *, int, int, int, int, int);
static void pcxe_put_char(struct tty_struct *, unsigned char);
static void pcxe_flush_chars(struct tty_struct *);
static void pcxx_error(int, char *);
static void pcxe_close(struct tty_struct *, struct file *);
static int pcxe_ioctl(struct tty_struct *, struct file *, unsigned int, unsigned long);
static void pcxe_set_termios(struct tty_struct *, struct termios *);
static int pcxe_write(struct tty_struct *, int, const unsigned char *, int);
static int pcxe_write_room(struct tty_struct *);
static int pcxe_chars_in_buffer(struct tty_struct *);
static void pcxe_flush_buffer(struct tty_struct *);
static void doevent(int);
static void receive_data(struct channel *);
static void pcxxparam(struct tty_struct *, struct channel *ch);
static void do_softint(void *);
static inline void pcxe_sched_event(struct channel *, int);
static void pcxe_start(struct tty_struct *);
static void pcxe_stop(struct tty_struct *);
static void pcxe_throttle(struct tty_struct *);
static void pcxe_unthrottle(struct tty_struct *);
static void digi_send_break(struct channel *ch, int msec);
static void shutdown(struct channel *);
static void setup_empty_event(struct tty_struct *tty, struct channel *ch);
static inline void memwinon(struct board_info *b, unsigned int win);
static inline void memwinoff(struct board_info *b, unsigned int win);
static inline void globalwinon(struct channel *ch);
static inline void rxwinon(struct channel *ch);
static inline void txwinon(struct channel *ch);
static inline void memoff(struct channel *ch);
static inline void assertgwinon(struct channel *ch);
static inline void assertmemoff(struct channel *ch);
static int pcxe_tiocmget(struct tty_struct *tty, struct file *file);
static int pcxe_tiocmset(struct tty_struct *tty, struct file *file,
			 unsigned int set, unsigned int clear);

#define TZ_BUFSZ 4096

/* function definitions */

/*****************************************************************************/

static void cleanup_board_resources(void)
{
	int crd, i;
	struct board_info *bd;
	struct channel *ch;

        for(crd = 0; crd < numcards; crd++) {
                bd = &boards[crd];
		ch = digi_channels + bd->first_minor;

		if (bd->region)
			release_region(bd->port, 4);

		for(i = 0; i < bd->numports; i++, ch++)
			if (ch->tmp_buf)
				kfree(ch->tmp_buf);
	}
}

static void __exit pcxe_cleanup(void)
{

	unsigned long	flags;
	int e1;

	printk(KERN_NOTICE "Unloading PC/Xx version %s\n", VERSION);

	save_flags(flags);
	cli();
	del_timer_sync(&pcxx_timer);

	if ((e1 = tty_unregister_driver(pcxe_driver)))
		printk("SERIAL: failed to unregister serial driver (%d)\n", e1);

	put_tty_driver(pcxe_driver);
	cleanup_board_resources();
	kfree(digi_channels);
	restore_flags(flags);
}

static inline struct channel *chan(register struct tty_struct *tty)
{
	if (tty) {
		register struct channel *ch=(struct channel *)tty->driver_data;
		if (ch >= digi_channels && ch < digi_channels+nbdevs) {
			if (ch->magic==PCXX_MAGIC)
				return ch;
		}
	}
	return NULL;
}

/* These inline routines are to turn board memory on and off */
static inline void memwinon(struct board_info *b, unsigned int win)
{
	if(b->type == PCXEVE)
		outb_p(FEPWIN|win, b->port+1);
	else
		outb_p(inb(b->port)|FEPMEM, b->port);
}

static inline void memwinoff(struct board_info *b, unsigned int win)
{
	outb_p(inb(b->port)&~FEPMEM, b->port);
	if(b->type == PCXEVE)
		outb_p(0, b->port + 1);
}

static inline void globalwinon(struct channel *ch)
{
	if(ch->board->type == PCXEVE)
		outb_p(FEPWIN, ch->board->port+1);
	else
		outb_p(FEPMEM, ch->board->port);
}

static inline void rxwinon(struct channel *ch)
{
	if(ch->rxwin == 0)
		outb_p(FEPMEM, ch->board->port);
	else 
		outb_p(ch->rxwin, ch->board->port+1);
}

static inline void txwinon(struct channel *ch)
{
	if(ch->txwin == 0)
		outb_p(FEPMEM, ch->board->port);
	else
		outb_p(ch->txwin, ch->board->port+1);
}

static inline void memoff(struct channel *ch)
{
	outb_p(0, ch->board->port);
	if(ch->board->type == PCXEVE)
		outb_p(0, ch->board->port+1);
}

static inline void assertgwinon(struct channel *ch)
{
	if(ch->board->type != PCXEVE)
		pcxxassert(inb(ch->board->port) & FEPMEM, "Global memory off");
}

static inline void assertmemoff(struct channel *ch)
{
	if(ch->board->type != PCXEVE)
		pcxxassert(!(inb(ch->board->port) & FEPMEM), "Memory on");
}

static inline void pcxe_sched_event(struct channel *info, int event)
{
	info->event |= 1 << event;
	schedule_work(&info->tqueue);
}

static void pcxx_error(int line, char *msg)
{
	printk("pcxx_error (DigiBoard): line=%d %s\n", line, msg);
}

static int pcxx_waitcarrier(struct tty_struct *tty,struct file *filp,struct channel *info)
{
	DECLARE_WAITQUEUE(wait, current);
	int	retval = 0;
	int	do_clocal = 0;

	if (tty->termios->c_cflag & CLOCAL)
		do_clocal = 1;

	/*
	 * Block waiting for the carrier detect and the line to become free
	 */

	retval = 0;
	add_wait_queue(&info->open_wait, &wait);
	info->count--;
	info->blocked_open++;

	for (;;) {
		cli();
		globalwinon(info);
		info->omodem |= DTR|RTS;
		fepcmd(info, SETMODEM, DTR|RTS, 0, 10, 1);
		memoff(info);
		sti();
		set_current_state(TASK_INTERRUPTIBLE);
		if(tty_hung_up_p(filp) || (info->asyncflags & ASYNC_INITIALIZED) == 0) {
			if(info->asyncflags & ASYNC_HUP_NOTIFY)
				retval = -EAGAIN;
			else
				retval = -ERESTARTSYS;	
			break;
		}
		if ((info->asyncflags & ASYNC_CLOSING) == 0 &&
			(do_clocal || (info->imodem & info->dcd)))
			break;
		if(signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&info->open_wait, &wait);

	if(!tty_hung_up_p(filp))
		info->count++;
	info->blocked_open--;

	return retval;
}	


int pcxe_open(struct tty_struct *tty, struct file * filp)
{
	volatile struct board_chan *bc;
	struct channel *ch;
	unsigned long flags;
	int line;
	int boardnum;
	int retval;

	line = tty->index;

	if(line < 0 || line >= nbdevs) {
		printk("line out of range in pcxe_open\n");
		tty->driver_data = NULL;
		return(-ENODEV);
	}

	for(boardnum=0;boardnum<numcards;boardnum++)
		if ((line >= boards[boardnum].first_minor) && 
			(line < boards[boardnum].first_minor + boards[boardnum].numports))
		break;

	if(boardnum >= numcards || boards[boardnum].status == DISABLED ||
		(line - boards[boardnum].first_minor) >= boards[boardnum].numports) {
		tty->driver_data = NULL;   /* Mark this device as 'down' */
		return(-ENODEV);
	}

	ch = digi_channels+line;

	if(ch->brdchan == 0) {
		tty->driver_data = NULL;
		return(-ENODEV);
	}

	/*
	 * If the device is in the middle of being closed, then block
	 * until it's done, and then try again.
	 */
	if(ch->asyncflags & ASYNC_CLOSING) {
		interruptible_sleep_on(&ch->close_wait);
		if(ch->asyncflags & ASYNC_HUP_NOTIFY)
			return -EAGAIN;
		else
			return -ERESTARTSYS;
	}

	save_flags(flags);
	cli();
	ch->count++;
	tty->driver_data = ch;
	ch->tty = tty;

	if ((ch->asyncflags & ASYNC_INITIALIZED) == 0) {
		unsigned int head;

		globalwinon(ch);
		ch->statusflags = 0;
		bc=ch->brdchan;
		ch->imodem = bc->mstat;
		head = bc->rin;
		bc->rout = head;
		ch->tty = tty;
		pcxxparam(tty,ch);
		ch->imodem = bc->mstat;
		bc->idata = 1;
		ch->omodem = DTR|RTS;
		fepcmd(ch, SETMODEM, DTR|RTS, 0, 10, 1);
		memoff(ch);
		ch->asyncflags |= ASYNC_INITIALIZED;
	}
	restore_flags(flags);

	if(ch->asyncflags & ASYNC_CLOSING) {
		interruptible_sleep_on(&ch->close_wait);
		if(ch->asyncflags & ASYNC_HUP_NOTIFY)
			return -EAGAIN;
		else
			return -ERESTARTSYS;
	}

	if (!(filp->f_flags & O_NONBLOCK)) {
		/* this has to be set in order for the "block until
		 * CD" code to work correctly.  i'm not sure under
		 * what circumstances asyncflags should be set to
		 * ASYNC_NORMAL_ACTIVE though
		 * brian@ilinx.com
		 */
		ch->asyncflags |= ASYNC_NORMAL_ACTIVE;
		if ((retval = pcxx_waitcarrier(tty, filp, ch)) != 0)
			return retval;
	}
	ch->asyncflags |= ASYNC_NORMAL_ACTIVE;
 	
	return 0;
} 

static void shutdown(struct channel *info)
{
	unsigned long flags;
	volatile struct board_chan *bc;
	struct tty_struct *tty;

	if (!(info->asyncflags & ASYNC_INITIALIZED)) 
		return;

	save_flags(flags);
	cli();
	globalwinon(info);

	bc = info->brdchan;
	if(bc)
		bc->idata = 0;

	tty = info->tty;

	/*
	 * If we're a modem control device and HUPCL is on, drop RTS & DTR.
	 */
	if(tty->termios->c_cflag & HUPCL) {
		info->omodem &= ~(RTS|DTR);
		fepcmd(info, SETMODEM, 0, DTR|RTS, 10, 1);
	}

	memoff(info);
	info->asyncflags &= ~ASYNC_INITIALIZED;
	restore_flags(flags);
}


static void pcxe_close(struct tty_struct * tty, struct file * filp)
{
	struct channel *info;

	if ((info=chan(tty))!=NULL) {
		unsigned long flags;
		save_flags(flags);
		cli();

		if(tty_hung_up_p(filp)) {
			/* flag that somebody is done with this module */
			restore_flags(flags);
			return;
		}
		/* this check is in serial.c, it won't hurt to do it here too */
		if ((tty->count == 1) && (info->count != 1)) {
			/*
			 * Uh, oh.  tty->count is 1, which means that the tty
			 * structure will be freed.  Info->count should always
			 * be one in these conditions.  If it's greater than
			 * one, we've got real problems, since it means the
			 * serial port won't be shutdown.
			 */
			printk("pcxe_close: bad serial port count; tty->count is 1, info->count is %d\n", info->count);
			info->count = 1;
		}
		if (info->count-- > 1) {
			restore_flags(flags);
			return;
		}
		if (info->count < 0) {
			info->count = 0;
		}

		info->asyncflags |= ASYNC_CLOSING;
	
		tty->closing = 1;
		if(info->asyncflags & ASYNC_INITIALIZED) {
			setup_empty_event(tty,info);		
			tty_wait_until_sent(tty, 3000); /* 30 seconds timeout */
		}
	
		if(tty->driver->flush_buffer)
			tty->driver->flush_buffer(tty);
		tty_ldisc_flush(tty);
		shutdown(info);
		tty->closing = 0;
		info->event = 0;
		info->tty = NULL;
		if(info->blocked_open) {
			if(info->close_delay) {
				msleep_interruptible(jiffies_to_msecs(info->close_delay));
			}
			wake_up_interruptible(&info->open_wait);
		}
		info->asyncflags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CLOSING);
		wake_up_interruptible(&info->close_wait);
		restore_flags(flags);
	}
}


void pcxe_hangup(struct tty_struct *tty)
{
	struct channel *ch;

	if ((ch=chan(tty))!=NULL) {
		unsigned long flags;

		save_flags(flags);
		cli();
		shutdown(ch);
		ch->event = 0;
		ch->count = 0;
		ch->tty = NULL;
		ch->asyncflags &= ~ASYNC_NORMAL_ACTIVE;
		wake_up_interruptible(&ch->open_wait);
		restore_flags(flags);
	}
}



static int pcxe_write(struct tty_struct * tty, const unsigned char *buf, int count)
{
	struct channel *ch;
	volatile struct board_chan *bc;
	int total, remain, size, stlen;
	unsigned int head, tail;
	unsigned long flags;
	/* printk("Entering pcxe_write()\n"); */

	if ((ch=chan(tty))==NULL)
		return 0;

	bc = ch->brdchan;
	size = ch->txbufsize;

	/*
	 * All data is now local
	 */

	total = 0;
	save_flags(flags);
	cli();
	globalwinon(ch);
	head = bc->tin & (size - 1);
	tail = bc->tout;
	if (tail != bc->tout)
		tail = bc->tout;
	tail &= (size - 1);
	if (head >= tail) {
		remain = size - (head - tail) - 1;
		stlen = size - head;
	}
	else {
		remain = tail - head - 1;
		stlen = remain;
	}
	count = min(remain, count);

	txwinon(ch);
	while (count > 0) {
		stlen = min(count, stlen);
		memcpy(ch->txptr + head, buf, stlen);
		buf += stlen;
		count -= stlen;
		total += stlen;
		head += stlen;
		if (head >= size) {
			head = 0;
			stlen = tail;
		}
	}
	ch->statusflags |= TXBUSY;
	globalwinon(ch);
	bc->tin = head;
	if ((ch->statusflags & LOWWAIT) == 0) {
		ch->statusflags |= LOWWAIT;
		bc->ilow = 1;
	}
	memoff(ch);
	restore_flags(flags);
	
	return(total);
}


static void pcxe_put_char(struct tty_struct *tty, unsigned char c)
{
	pcxe_write(tty, 0, &c, 1);
	return;
}


static int pcxe_write_room(struct tty_struct *tty)
{
	struct channel *ch;
	int remain;

	remain = 0;
	if ((ch=chan(tty))!=NULL) {
		volatile struct board_chan *bc;
		unsigned int head, tail;
		unsigned long flags;

		save_flags(flags);
		cli();
		globalwinon(ch);

		bc = ch->brdchan;
		head = bc->tin & (ch->txbufsize - 1);
		tail = bc->tout;
		if (tail != bc->tout)
			tail = bc->tout;
		tail &= (ch->txbufsize - 1);

		if((remain = tail - head - 1) < 0 )
			remain += ch->txbufsize;

		if (remain && (ch->statusflags & LOWWAIT) == 0) {
			ch->statusflags |= LOWWAIT;
			bc->ilow = 1;
		}
		memoff(ch);
		restore_flags(flags);
	}

	return remain;
}


static int pcxe_chars_in_buffer(struct tty_struct *tty)
{
	int chars;
	unsigned int ctail, head, tail;
	int remain;
	unsigned long flags;
	struct channel *ch;
	volatile struct board_chan *bc;

	if ((ch=chan(tty))==NULL)
		return(0);

	save_flags(flags);
	cli();
	globalwinon(ch);

	bc = ch->brdchan;
	tail = bc->tout;
	head = bc->tin;
	ctail = ch->mailbox->cout;
	if(tail == head && ch->mailbox->cin == ctail && bc->tbusy == 0)
		chars = 0;
	else {
		head = bc->tin & (ch->txbufsize - 1);
		tail &= (ch->txbufsize - 1);
		if((remain = tail - head - 1) < 0 )
			remain += ch->txbufsize;

		chars = (int)(ch->txbufsize - remain);

		/* 
		 * Make it possible to wakeup anything waiting for output
		 * in tty_ioctl.c, etc.
		 */
		if(!(ch->statusflags & EMPTYWAIT))
			setup_empty_event(tty,ch);
	}

	memoff(ch);
	restore_flags(flags);

	return(chars);
}


static void pcxe_flush_buffer(struct tty_struct *tty)
{
	unsigned int tail;
	volatile struct board_chan *bc;
	struct channel *ch;
	unsigned long flags;

	if ((ch=chan(tty))==NULL)
		return;

	save_flags(flags);
	cli();

	globalwinon(ch);
	bc = ch->brdchan;
	tail = bc->tout;
	fepcmd(ch, STOUT, (unsigned) tail, 0, 0, 0);

	memoff(ch);
	restore_flags(flags);

	tty_wakeup(tty);
}

static void pcxe_flush_chars(struct tty_struct *tty)
{
	struct channel * ch;

	if ((ch=chan(tty))!=NULL) {
		unsigned long flags;

		save_flags(flags);
		cli();
		if ((ch->statusflags & TXBUSY) && !(ch->statusflags & EMPTYWAIT))
			setup_empty_event(tty,ch);
		restore_flags(flags);
	}
}

#ifndef MODULE

/*
 * Driver setup function when linked into the kernel to optionally parse multible
 * "digi="-lines and initialize the driver at boot time. No probing.
 */
void __init pcxx_setup(char *str, int *ints)
{

	struct board_info board;
	int               i, j, last;
	char              *temp, *t2;
	unsigned          len;

	numcards=0;

	memset(&board, 0, sizeof(board));

	for(last=0,i=1;i<=ints[0];i++)
		switch(i)
		{
			case 1:
				board.status = ints[i];
				last = i;
				break;

			case 2:
				board.type = ints[i];
				last = i;
				break;

			case 3:
				board.altpin = ints[i];
				last = i;
				break;

			case 4:
				board.numports = ints[i];
				last = i;
				break;

			case 5:
				board.port = ints[i];
				last = i;
				break;

			case 6:
				board.membase = ints[i];
				last = i;
				break;

			default:
				printk("PC/Xx: Too many integer parms\n");
				return;
		}

	while (str && *str) 
	{
		/* find the next comma or terminator */
		temp = str;
		while (*temp && (*temp != ','))
			temp++;

		if (!*temp)
			temp = NULL;
		else
			*temp++ = 0;

		i = last + 1;

		switch(i)
		{
			case 1:
				len = strlen(str);
				if (strncmp("Disable", str, len) == 0) 
					board.status = 0;
				else
					if (strncmp("Enable", str, len) == 0)
						board.status = 1;
					else
					{
						printk("PC/Xx: Invalid status %s\n", str);
						return;
					}
				last = i;
				break;

			case 2:
				for(j=0;j<PCXX_NUM_TYPES;j++)
					if (strcmp(board_desc[j], str) == 0)
						break;

				if (i<PCXX_NUM_TYPES) 
					board.type = j;
				else
				{
					printk("PC/Xx: Invalid board name: %s\n", str);
					return;
				}
				last = i;
				break;

			case 3:
				len = strlen(str);
				if (strncmp("Disable", str, len) == 0) 
					board.altpin = 0;
				else
					if (strncmp("Enable", str, len) == 0)
						board.altpin = 1;
					else
					{
						printk("PC/Xx: Invalid altpin %s\n", str);
						return;
					}
				last = i;
				break;

			case 4:
				t2 = str;
				while (isdigit(*t2))
					t2++;

				if (*t2)
				{
					printk("PC/Xx: Invalid port count %s\n", str);
					return;
				}

				board.numports = simple_strtoul(str, NULL, 0);
				last = i;
				break;

			case 5:
				t2 = str;
				while (isxdigit(*t2))
					t2++;

				if (*t2)
				{
					printk("PC/Xx: Invalid io port address %s\n", str);
					return;
				}

				board.port = simple_strtoul(str, NULL, 16);
				last = i;
				break;

			case 6:
				t2 = str;
				while (isxdigit(*t2))
					t2++;

				if (*t2)
				{
					printk("PC/Xx: Invalid memory base %s\n", str);
					return;
				}

				board.membase = simple_strtoul(str, NULL, 16);
				last = i;
				break;

			default:
				printk("PC/Xx: Too many string parms\n");
				return;
		}
		str = temp;
	}

	if (last < 6)  
	{
		printk("PC/Xx: Insufficient parms specified\n");
		return;
	}
 
        /* I should REALLY validate the stuff here */

	memcpy(&boards[numcards],&board, sizeof(board));
	printk("PC/Xx: Added board %i, %s %s %i ports at 0x%4.4X base 0x%6.6X\n", 
		numcards, board_desc[board.type], board_mem[board.type], 
		board.numports, board.port, (unsigned int) board.membase);

	/* keep track of my initial minor number */
        if (numcards)
		boards[numcards].first_minor = boards[numcards-1].first_minor + boards[numcards-1].numports;
	else
		boards[numcards].first_minor = 0;

	/* yeha!  string parameter was successful! */
	numcards++;
}
#endif

static struct tty_operations pcxe_ops = {
	.open = pcxe_open,
	.close = pcxe_close,
	.write = pcxe_write,
	.put_char = pcxe_put_char,
	.flush_chars = pcxe_flush_chars,
	.write_room = pcxe_write_room,
	.chars_in_buffer = pcxe_chars_in_buffer,
	.flush_buffer = pcxe_flush_buffer,
	.ioctl = pcxe_ioctl,
	.throttle = pcxe_throttle,
	.unthrottle = pcxe_unthrottle,
	.set_termios = pcxe_set_termios,
	.stop = pcxe_stop,
	.start = pcxe_start,
	.hangup = pcxe_hangup,
	.tiocmget = pcxe_tiocmget,
	.tiocmset = pcxe_tiocmset,
};

/*
 * function to initialize the driver with the given parameters, which are either
 * the default values from this file or the parameters given at boot.
 */
static int __init pcxe_init(void)
{
	ulong memory_seg=0, memory_size=0;
	int lowwater, enabled_cards=0, i, crd, shrinkmem=0, topwin = 0xff00L, botwin=0x100L;
	int ret = -ENOMEM;
	unchar *fepos, *memaddr, *bios, v;
	volatile struct global_data *gd;
	volatile struct board_chan *bc;
	struct board_info *bd;
	struct channel *ch;

	printk(KERN_NOTICE "Digiboard PC/X{i,e,eve} driver v%s\n", VERSION);

#ifdef MODULE
	for (i = 0; i < MAX_DIGI_BOARDS; i++) {
		if (io[i]) {
			numcards = 0;
			break;
		}
	}
	if (numcards == 0) {
		int first_minor = 0;

		for (i = 0; i < MAX_DIGI_BOARDS; i++) {
			if (io[i] == 0) {
				boards[i].port    = 0;
				boards[i].status  = DISABLED;
			}
			else {
				boards[i].port         = (ushort)io[i];
				boards[i].status       = ENABLED;
				boards[i].first_minor  = first_minor;
				numcards=i+1;
			}
			if (membase[i])
				boards[i].membase = (ulong)membase[i];
			else
				boards[i].membase = 0xD0000;

			if (memsize[i])
				boards[i].memsize = (ulong)(memsize[i] * 1024);
			else
				boards[i].memsize = 0;

			if (altpin[i])
				boards[i].altpin  = ON;
			else
				boards[i].altpin  = OFF;

			if (numports[i])
				boards[i].numports  = (ushort)numports[i];
			else
				boards[i].numports  = 16;

			boards[i].region = NULL;
			first_minor += boards[i].numports;
		}
	}
#endif

	if (numcards <= 0)
	{
		printk("PC/Xx: No cards configured, driver not active.\n");
		return -EIO;
	}
#if 1
	if (debug)
	    for (i = 0; i < numcards; i++) {
		    printk("Card %d:status=%d, port=0x%x, membase=0x%lx, memsize=0x%lx, altpin=%d, numports=%d, first_minor=%d\n",
			    i+1,
			    boards[i].status,
			    boards[i].port,
			    boards[i].membase,
			    boards[i].memsize,
			    boards[i].altpin,
			    boards[i].numports,
			    boards[i].first_minor);
	    }
#endif

	for (i=0;i<numcards;i++)
		nbdevs += boards[i].numports;

	if (nbdevs <= 0)
	{
		printk("PC/Xx: No devices activated, driver not active.\n");
		return -EIO;
	}

	pcxe_driver = alloc_tty_driver(nbdevs);
	if (!pcxe_driver)
		return -ENOMEM;

	/*
	 * this turns out to be more memory efficient, as there are no 
	 * unused spaces.
	 */
	digi_channels = kmalloc(sizeof(struct channel) * nbdevs, GFP_KERNEL);
	if (!digi_channels) {
		printk(KERN_ERR "Unable to allocate digi_channel struct\n");
		put_tty_driver(pcxe_driver);
		return -ENOMEM;
	}
	memset(digi_channels, 0, sizeof(struct channel) * nbdevs);

	init_timer(&pcxx_timer);
	pcxx_timer.function = pcxxpoll;

	pcxe_driver->owner = THIS_MODULE;
	pcxe_driver->name = "ttyD";
	pcxe_driver->devfs_name = "pcxe/";
	pcxe_driver->major = DIGI_MAJOR; 
	pcxe_driver->minor_start = 0;
	pcxe_driver->type = TTY_DRIVER_TYPE_SERIAL;
	pcxe_driver->subtype = SERIAL_TYPE_NORMAL;
	pcxe_driver->init_termios = tty_std_termios;
	pcxe_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL;
	pcxe_driver->flags = TTY_DRIVER_REAL_RAW;
	tty_set_operations(pcxe_driver, &pcxe_ops);

	for(crd=0; crd < numcards; crd++) {
		bd = &boards[crd];
		outb(FEPRST, bd->port);
		mdelay(1);

		for(i=0; (inb(bd->port) & FEPMASK) != FEPRST; i++) {
			if(i > 100) {
				printk("PC/Xx: Board not found at port 0x%x! Check switch settings.\n",
					bd->port);
				bd->status = DISABLED;
				break;
			}
#ifdef MODULE
			schedule();
#endif
			mdelay(10);
		}
		if(bd->status == DISABLED)
			continue;

		v = inb(bd->port);

		if((v & 0x1) == 0x1) {
			if((v & 0x30) == 0) {        /* PC/Xi 64K card */
				memory_seg = 0xf000;
				memory_size = 0x10000;
			} 

			if((v & 0x30) == 0x10) {     /* PC/Xi 128K card */
				memory_seg = 0xe000;
				memory_size = 0x20000;
			}
			
			if((v & 0x30) == 0x20) {     /* PC/Xi 256K card */
				memory_seg = 0xc000;
				memory_size = 0x40000;
			}

			if((v & 0x30) == 0x30) {     /* PC/Xi 512K card */
				memory_seg = 0x8000;
				memory_size = 0x80000;
			}
			bd->type = PCXI;
		} else {
			if((v & 0x1) == 0x1) {
				bd->status = DISABLED;   /* PC/Xm unsupported card */
				printk("PC/Xx: PC/Xm at 0x%x not supported!!\n", bd->port);
				continue;
			} else {
				if(v & 0xC0) {    
					topwin = 0x1f00L;
					outb((((ulong)bd->membase>>8) & 0xe0) | 0x10, bd->port+2);
					outb(((ulong)bd->membase>>16) & 0xff, bd->port+3);
					bd->type = PCXEVE; /* PC/Xe 8K card */
				} else { 
					bd->type = PCXE;    /* PC/Xe 64K card */
				}
					
				memory_seg = 0xf000;
				memory_size = 0x10000;
			}
		}
		if (verbose)
			printk("Configuring card %d as a %s %ldK card. io=0x%x, mem=%lx-%lx\n",
				crd+1, board_desc[bd->type], memory_size/1024,
				bd->port,bd->membase,bd->membase+memory_size-1);

		if (boards[crd].memsize == 0)
			boards[crd].memsize = memory_size;
		else
			if (boards[crd].memsize != memory_size) {
			    printk("PC/Xx: memory size mismatch:supplied=%lx(%ldK) probed=%ld(%ldK)\n",
				    boards[crd].memsize, boards[crd].memsize / 1024,
				    memory_size, memory_size / 1024);
			    continue;
			}

		memaddr = (unchar *)phys_to_virt(bd->membase);

		if (verbose)
			printk("Resetting board and testing memory access:");

		outb(FEPRST|FEPMEM, bd->port);

		for(i=0; (inb(bd->port) & FEPMASK) != (FEPRST|FEPMEM); i++) {
			if(i > 1000) {
				printk("\nPC/Xx: %s not resetting at port 0x%x! Check switch settings.\n",
					board_desc[bd->type], bd->port);
				bd->status = DISABLED;
				break;
			}
#ifdef MODULE
			schedule();
#endif
			mdelay(1);
		}
		if(bd->status == DISABLED)
			continue;

		memwinon(bd,0);
		*(ulong *)(memaddr + botwin) = 0xa55a3cc3;
		*(ulong *)(memaddr + topwin) = 0x5aa5c33c;

		if(*(ulong *)(memaddr + botwin) != 0xa55a3cc3 ||
					*(ulong *)(memaddr + topwin) != 0x5aa5c33c) {
			printk("PC/Xx: Failed memory test at %lx for %s at port %x, check switch settings.\n",
				bd->membase, board_desc[bd->type], bd->port);
			bd->status = DISABLED;
			continue;
		}
		if (verbose)
			printk(" done.\n");

		for(i=0; i < 16; i++) {
			memaddr[MISCGLOBAL+i] = 0;
		}

		if(bd->type == PCXI || bd->type == PCXE) {
			bios = memaddr + BIOSCODE + ((0xf000 - memory_seg) << 4);

			if (verbose)
				printk("Downloading BIOS to 0x%lx:", virt_to_phys(bios));

			memcpy(bios, pcxx_bios, pcxx_nbios);

			if (verbose)
				printk(" done.\n");

			outb(FEPMEM, bd->port);

			if (verbose)
				printk("Waiting for BIOS to become ready");

			for(i=1; i <= 30; i++) {
				if(*(ushort *)((ulong)memaddr + MISCGLOBAL) == *(ushort *)"GD" ) {
					goto load_fep;
				}
				if (verbose) {
					printk(".");
					if (i % 50 == 0)
						printk("\n");
				}
#ifdef MODULE
				schedule();
#endif
				mdelay(50);
			}

			printk("\nPC/Xx: BIOS download failed for board at 0x%x(addr=%lx-%lx)!\n",
							bd->port, bd->membase, bd->membase+bd->memsize);
			bd->status = DISABLED;
			continue;
		}

		if(bd->type == PCXEVE) {
			bios = memaddr + (BIOSCODE & 0x1fff);
			memwinon(bd,0xff);
			
			memcpy(bios, pcxx_bios, pcxx_nbios);

			outb(FEPCLR, bd->port);
			memwinon(bd,0);

			for(i=0; i <= 1000; i++) {
				if(*(ushort *)((ulong)memaddr + MISCGLOBAL) == *(ushort *)"GD" ) {
					goto load_fep;
				}
				if (verbose) {
					printk(".");
					if (i % 50 == 0)
						printk("\n");
				}
#ifdef MODULE
				schedule();
#endif
				mdelay(10);
			}

			printk("\nPC/Xx: BIOS download failed on the %s at 0x%x!\n",
				board_desc[bd->type], bd->port);
			bd->status = DISABLED;
			continue;
		}

load_fep:
		fepos = memaddr + FEPCODE;
		if(bd->type == PCXEVE)
			fepos = memaddr + (FEPCODE & 0x1fff);

		if (verbose)
			printk(" ok.\nDownloading FEP/OS to 0x%lx:", virt_to_phys(fepos));

		memwinon(bd, (FEPCODE >> 13));
		memcpy(fepos, pcxx_cook, pcxx_ncook);
		memwinon(bd, 0);

		if (verbose)
			printk(" done.\n");

		*(ushort *)((ulong)memaddr + MBOX +  0) = 2;
		*(ushort *)((ulong)memaddr + MBOX +  2) = memory_seg + FEPCODESEG;
		*(ushort *)((ulong)memaddr + MBOX +  4) = 0;
		*(ushort *)((ulong)memaddr + MBOX +  6) = FEPCODESEG;
		*(ushort *)((ulong)memaddr + MBOX +  8) = 0;
		*(ushort *)((ulong)memaddr + MBOX + 10) = pcxx_ncook;

		outb(FEPMEM|FEPINT, bd->port);
		outb(FEPMEM, bd->port);

		for(i=0; *(ushort *)((ulong)memaddr + MBOX); i++) {
			if(i > 2000) {
				printk("PC/Xx: Command failed for the %s at 0x%x!\n",
					board_desc[bd->type], bd->port);
				bd->status = DISABLED;
				break;
			}
#ifdef MODULE
			schedule();
#endif
			mdelay(1);
		}

		if(bd->status == DISABLED)
			continue;

		if (verbose)
			printk("Waiting for FEP/OS to become ready");

		*(ushort *)(memaddr + FEPSTAT) = 0;
		*(ushort *)(memaddr + MBOX + 0) = 1;
		*(ushort *)(memaddr + MBOX + 2) = FEPCODESEG;
		*(ushort *)(memaddr + MBOX + 4) = 0x4L;

		outb(FEPINT, bd->port);
		outb(FEPCLR, bd->port);
		memwinon(bd, 0);

		for(i=1; *(ushort *)((ulong)memaddr + FEPSTAT) != *(ushort *)"OS"; i++) {
			if(i > 1000) {
				printk("\nPC/Xx: FEP/OS download failed on the %s at 0x%x!\n",
					board_desc[bd->type], bd->port);
				bd->status = DISABLED;
				break;
			}
			if (verbose) {
				printk(".");
				if (i % 50 == 0)
					printk("\n%5d",i/50);
			}
#ifdef MODULE
			schedule();
#endif
			mdelay(1);
		}
		if(bd->status == DISABLED)
			continue;

		if (verbose)
			printk(" ok.\n");

		ch = digi_channels+bd->first_minor;
		pcxxassert(ch < digi_channels+nbdevs, "ch out of range");

		bc = (volatile struct board_chan *)((ulong)memaddr + CHANSTRUCT);
		gd = (volatile struct global_data *)((ulong)memaddr + GLOBAL);

		if((bd->type == PCXEVE) && (*(ushort *)((ulong)memaddr+NPORT) < 3))
			shrinkmem = 1;

		bd->region = request_region(bd->port, 4, "PC/Xx");

		if (!bd->region) {
			printk(KERN_ERR "I/O port 0x%x is already used\n", bd->port);
			ret = -EBUSY;
			goto cleanup_boards;
		}

		for(i=0; i < bd->numports; i++, ch++, bc++) {
			if(((ushort *)((ulong)memaddr + PORTBASE))[i] == 0) {
				ch->brdchan = 0;
				continue;
			}
			ch->brdchan = bc;
			ch->mailbox = gd;
			INIT_WORK(&ch->tqueue, do_softint, ch);
			ch->board = &boards[crd];
#ifdef DEFAULT_HW_FLOW
			ch->digiext.digi_flags = RTSPACE|CTSPACE;
#endif
			if(boards[crd].altpin) {
				ch->dsr = CD;
				ch->dcd = DSR;
				ch->digiext.digi_flags |= DIGI_ALTPIN;
			} else { 
				ch->dcd = CD;
				ch->dsr = DSR;
			}

			ch->magic = PCXX_MAGIC;
			ch->boardnum = crd;
			ch->channelnum = i;

			ch->dev = bd->first_minor + i;
			ch->tty = 0;

			if(shrinkmem) {
				fepcmd(ch, SETBUFFER, 32, 0, 0, 0);
				shrinkmem = 0;
			}
			
			if(bd->type != PCXEVE) {
				ch->txptr = memaddr+((bc->tseg-memory_seg) << 4);
				ch->rxptr = memaddr+((bc->rseg-memory_seg) << 4);
				ch->txwin = ch->rxwin = 0;
			} else {
				ch->txptr = memaddr+(((bc->tseg-memory_seg) << 4) & 0x1fff);
				ch->txwin = FEPWIN | ((bc->tseg-memory_seg) >> 9);
				ch->rxptr = memaddr+(((bc->rseg-memory_seg) << 4) & 0x1fff);
				ch->rxwin = FEPWIN | ((bc->rseg-memory_seg) >>9 );
			}

			ch->txbufsize = bc->tmax + 1;
			ch->rxbufsize = bc->rmax + 1;
			ch->tmp_buf = kmalloc(ch->txbufsize,GFP_KERNEL);
			init_MUTEX(&ch->tmp_buf_sem);

			if (!ch->tmp_buf) {
				printk(KERN_ERR "Unable to allocate memory for temp buffers\n");
				goto cleanup_boards;
			}

			lowwater = ch->txbufsize >= 2000 ? 1024 : ch->txbufsize/2;
			fepcmd(ch, STXLWATER, lowwater, 0, 10, 0);
			fepcmd(ch, SRXLWATER, ch->rxbufsize/4, 0, 10, 0);
			fepcmd(ch, SRXHWATER, 3 * ch->rxbufsize/4, 0, 10, 0);

			bc->edelay = 100;
			bc->idata = 1;

			ch->startc = bc->startc;
			ch->stopc = bc->stopc;
			ch->startca = bc->startca;
			ch->stopca = bc->stopca;

			ch->fepcflag = 0;
			ch->fepiflag = 0;
			ch->fepoflag = 0;
			ch->fepstartc = 0;
			ch->fepstopc = 0;
			ch->fepstartca = 0;
			ch->fepstopca = 0;

			ch->close_delay = 50;
			ch->count = 0;
			ch->blocked_open = 0;
			init_waitqueue_head(&ch->open_wait);
			init_waitqueue_head(&ch->close_wait);
			ch->asyncflags = 0;
		}

		if (verbose)
		    printk("Card No. %d ready: %s (%s) I/O=0x%x Mem=0x%lx Ports=%d\n", 
			    crd+1, board_desc[bd->type], board_mem[bd->type], bd->port, 
			    bd->membase, bd->numports);
		else
		    printk("PC/Xx: %s (%s) I/O=0x%x Mem=0x%lx Ports=%d\n", 
			    board_desc[bd->type], board_mem[bd->type], bd->port, 
			    bd->membase, bd->numports);

		memwinoff(bd, 0);
		enabled_cards++;
	}

	if (enabled_cards <= 0) {
		printk(KERN_NOTICE "PC/Xx: No cards enabled, no driver.\n");
		ret = -EIO;
		goto cleanup_boards;
	}

	ret = tty_register_driver(pcxe_driver);
	if(ret) {
		printk(KERN_ERR "Couldn't register PC/Xe driver\n");
		goto cleanup_boards;
	}

	/*
	 * Start up the poller to check for events on all enabled boards
	 */
	mod_timer(&pcxx_timer, HZ/25);

	if (verbose)
		printk(KERN_NOTICE "PC/Xx: Driver with %d card(s) ready.\n", enabled_cards);

	return 0;
cleanup_boards:
	cleanup_board_resources();
	kfree(digi_channels);
	put_tty_driver(pcxe_driver);
	return ret;
}

module_init(pcxe_init)
module_exit(pcxe_cleanup)

static void pcxxpoll(unsigned long dummy)
{
	unsigned long flags;
	int crd;
	volatile unsigned int head, tail;
	struct channel *ch;
	struct board_info *bd;

	save_flags(flags);
	cli();

	for(crd=0; crd < numcards; crd++) {
		bd = &boards[crd];

		ch = digi_channels+bd->first_minor;

		if(bd->status == DISABLED)
			continue;

		assertmemoff(ch);

		globalwinon(ch);
		head = ch->mailbox->ein;
		tail = ch->mailbox->eout;

		if(head != tail)
			doevent(crd);

		memoff(ch);
	}

	mod_timer(&pcxx_timer, jiffies + HZ/25);
	restore_flags(flags);
}

static void doevent(int crd)
{
	volatile struct board_info *bd;
	static struct tty_struct *tty;
	volatile struct board_chan *bc;
	volatile unchar *eventbuf;
	volatile unsigned int head;
	volatile unsigned int tail;
	struct channel *ch;
	struct channel *chan0;
	int channel, event, mstat, lstat;

	bd = &boards[crd];

	chan0 = digi_channels+bd->first_minor;
	pcxxassert(chan0 < digi_channels+nbdevs, "ch out of range");


	assertgwinon(chan0);

	while ((tail = chan0->mailbox->eout) != (head = chan0->mailbox->ein)) {
		assertgwinon(chan0);
		eventbuf = (volatile unchar *)phys_to_virt(bd->membase + tail + ISTART);
		channel = eventbuf[0];
		event = eventbuf[1];
		mstat = eventbuf[2];
		lstat = eventbuf[3];

		ch=chan0+channel;

		if ((unsigned)channel >= bd->numports || !ch) { 
			printk("physmem=%lx, tail=%x, head=%x\n", bd->membase, tail, head);
			printk("doevent(%x) channel %x, event %x, mstat %x, lstat %x\n",
					crd, (unsigned)channel, event, (unsigned)mstat, lstat);
			if(channel >= bd->numports)
				ch = chan0;
			bc = ch->brdchan;
			goto next;
		}
		if ((bc = ch->brdchan) == NULL)
			goto next;

		if (event & DATA_IND) {
			receive_data(ch);
			assertgwinon(ch);
		}

		if (event & MODEMCHG_IND) {
			ch->imodem = mstat;
			if (ch->asyncflags & ASYNC_NORMAL_ACTIVE) {
				if (ch->asyncflags & ASYNC_CHECK_CD) {
					if (mstat & ch->dcd) {
						wake_up_interruptible(&ch->open_wait);
					} else {
						pcxe_sched_event(ch, PCXE_EVENT_HANGUP);
					}
				}
			}
		}

		tty = ch->tty;

		if (tty) {

			if (event & BREAK_IND) {
				tty->flip.count++;
				*tty->flip.flag_buf_ptr++ = TTY_BREAK;
				*tty->flip.char_buf_ptr++ = 0;
#if 0
				if (ch->asyncflags & ASYNC_SAK)
					do_SAK(tty);
#endif
				tty_schedule_flip(tty); 
			}

			if (event & LOWTX_IND) {
				if (ch->statusflags & LOWWAIT) {
					ch->statusflags &= ~LOWWAIT;
					tty_wakeup(tty);
				}
			}

			if (event & EMPTYTX_IND) {
				ch->statusflags &= ~TXBUSY;
				if (ch->statusflags & EMPTYWAIT) {
					ch->statusflags &= ~EMPTYWAIT;
					tty_wakeup(tty);
				}
			}
		}

	next:
		globalwinon(ch);
		if(!bc) printk("bc == NULL in doevent!\n");
		else bc->idata = 1;

		chan0->mailbox->eout = (tail+4) & (IMAX-ISTART-4);
		globalwinon(chan0);
	}

}


static void 
fepcmd(struct channel *ch, int cmd, int word_or_byte, int byte2, int ncmds,
						int bytecmd)
{
	unchar *memaddr;
	unsigned int head, tail;
	long count;
	int n;

	if(ch->board->status == DISABLED)
		return;

	assertgwinon(ch);

	memaddr = (unchar *)phys_to_virt(ch->board->membase);
	head = ch->mailbox->cin;

	if(head >= (CMAX-CSTART) || (head & 03)) {
		printk("line %d: Out of range, cmd=%x, head=%x\n", __LINE__, cmd, head);
		return;
	}

	if(bytecmd) {
		*(unchar *)(memaddr+head+CSTART+0) = cmd;

		*(unchar *)(memaddr+head+CSTART+1) = ch->dev - ch->board->first_minor;

		*(unchar *)(memaddr+head+CSTART+2) = word_or_byte;
		*(unchar *)(memaddr+head+CSTART+3) = byte2;
	} else {
		*(unchar *)(memaddr+head+CSTART+0) = cmd;

		*(unchar *)(memaddr+head+CSTART+1) = ch->dev - ch->board->first_minor;
		*(ushort*)(memaddr+head+CSTART+2) = word_or_byte;
	}

	head = (head+4) & (CMAX-CSTART-4);
	ch->mailbox->cin = head;

	count = FEPTIMEOUT;

	while(1) {
		count--;
		if(count == 0) {
			printk("Fep not responding in fepcmd()\n");
			return;
		}

		head = ch->mailbox->cin;
		tail = ch->mailbox->cout;

		n = (head-tail) & (CMAX-CSTART-4);

		if(n <= ncmds * (sizeof(short)*4))
			break;
		/* Seems not to be good here: schedule(); */
	}
}


static unsigned termios2digi_c(struct channel *ch, unsigned cflag)
{
	unsigned res = 0;
	if (cflag & CBAUDEX)
	{
		ch->digiext.digi_flags |= DIGI_FAST;
		res |= FEP_HUPCL;
		/* This gets strange but if we don't do this we will get 78600
		 * instead of 115200. 57600 is mapped to 50 baud yielding 57600 in
		 * FAST mode. 115200 is mapped to 75. We need to map it to 110 to
		 * do 115K
		 */
		if (cflag & B115200) res|=1;
	}
	else ch->digiext.digi_flags &= ~DIGI_FAST;
	res |= cflag & (CBAUD | PARODD | PARENB | CSTOPB | CSIZE | CLOCAL);
	return res;
}

static unsigned termios2digi_i(struct channel *ch, unsigned iflag)
{
	unsigned res = iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK|ISTRIP|IXON|IXANY|IXOFF);
	
	if(ch->digiext.digi_flags & DIGI_AIXON)
		res |= IAIXON;
	return res;
}

static unsigned termios2digi_h(struct channel *ch, unsigned cflag)
{
	unsigned res = 0;

	if(cflag & CRTSCTS) {
		ch->digiext.digi_flags |= (RTSPACE|CTSPACE);
		res |= (CTS | RTS);
	}
	if(ch->digiext.digi_flags & RTSPACE)
		res |= RTS;
	if(ch->digiext.digi_flags & DTRPACE)
		res |= DTR;
	if(ch->digiext.digi_flags & CTSPACE)
		res |= CTS;
	if(ch->digiext.digi_flags & DSRPACE)
		res |= ch->dsr;
	if(ch->digiext.digi_flags & DCDPACE)
		res |= ch->dcd;

	if (res & RTS)
		ch->digiext.digi_flags |= RTSPACE;
	if (res & CTS)
		ch->digiext.digi_flags |= CTSPACE;

	return res;
}

static void pcxxparam(struct tty_struct *tty, struct channel *ch)
{
	volatile struct board_chan *bc;
	unsigned int head;
	unsigned mval, hflow, cflag, iflag;
	struct termios *ts;

	bc = ch->brdchan;
	assertgwinon(ch);
	ts = tty->termios;

	if((ts->c_cflag & CBAUD) == 0) {
		head = bc->rin;
		bc->rout = head;
		head = bc->tin;
		fepcmd(ch, STOUT, (unsigned) head, 0, 0, 0);
		mval = 0;
	} else {

		cflag = termios2digi_c(ch, ts->c_cflag);

		if(cflag != ch->fepcflag) {
			ch->fepcflag = cflag;
			fepcmd(ch, SETCTRLFLAGS, (unsigned) cflag, 0, 0, 0);
		}

		if(cflag & CLOCAL)
			ch->asyncflags &= ~ASYNC_CHECK_CD;
		else {
			ch->asyncflags |= ASYNC_CHECK_CD;
		}

		mval = DTR | RTS;
	}

	iflag = termios2digi_i(ch, ts->c_iflag);

	if(iflag != ch->fepiflag) {
		ch->fepiflag = iflag;
		fepcmd(ch, SETIFLAGS, (unsigned int) ch->fepiflag, 0, 0, 0);
	}

	bc->mint = ch->dcd;
	if((ts->c_cflag & CLOCAL) || (ch->digiext.digi_flags & DIGI_FORCEDCD))
		if(ch->digiext.digi_flags & DIGI_FORCEDCD)
			bc->mint = 0;

	ch->imodem = bc->mstat;

	hflow = termios2digi_h(ch, ts->c_cflag);

	if(hflow != ch->hflow) {
		ch->hflow = hflow;
		fepcmd(ch, SETHFLOW, hflow, 0xff, 0, 1);
	}

	/* mval ^= ch->modemfake & (mval ^ ch->modem); */

	if(ch->omodem != mval) {
		ch->omodem = mval;
		fepcmd(ch, SETMODEM, mval, RTS|DTR, 0, 1);
	}

	if(ch->startc != ch->fepstartc || ch->stopc != ch->fepstopc) {
		ch->fepstartc = ch->startc;
		ch->fepstopc = ch->stopc;
		fepcmd(ch, SONOFFC, ch->fepstartc, ch->fepstopc, 0, 1);
	}

	if(ch->startca != ch->fepstartca || ch->stopca != ch->fepstopca) {
		ch->fepstartca = ch->startca;
		ch->fepstopca = ch->stopca;
		fepcmd(ch, SAUXONOFFC, ch->fepstartca, ch->fepstopca, 0, 1);
	}
}


static void receive_data(struct channel *ch)
{
	volatile struct board_chan *bc;
	struct tty_struct *tty;
	unsigned int tail, head, wrapmask;
	int n;
	int piece;
	struct termios *ts=0;
	unchar *rptr;
	int rc;
	int wrapgap;

    globalwinon(ch);

	if (ch->statusflags & RXSTOPPED)
		return;

	tty = ch->tty;
	if(tty)
		ts = tty->termios;

	bc = ch->brdchan;

	if(!bc) {
		printk("bc is NULL in receive_data!\n");
		return;
	}

	wrapmask = ch->rxbufsize - 1;

	head = bc->rin;
	head &= wrapmask;
	tail = bc->rout & wrapmask;

	n = (head-tail) & wrapmask;

	if(n == 0)
		return;

	/*
	 * If CREAD bit is off or device not open, set TX tail to head
	 */
	if(!tty || !ts || !(ts->c_cflag & CREAD)) {
		bc->rout = head;
		return;
	}

	if(tty->flip.count == TTY_FLIPBUF_SIZE) {
		/* printk("tty->flip.count = TTY_FLIPBUF_SIZE\n"); */
		return;
	}

	if(bc->orun) {
		bc->orun = 0;
		printk("overrun! DigiBoard device %s\n", tty->name);
	}

	rxwinon(ch);
	rptr = tty->flip.char_buf_ptr;
	rc = tty->flip.count;
	while(n > 0) {
		wrapgap = (head >= tail) ? head - tail : ch->rxbufsize - tail;
		piece = (wrapgap < n) ? wrapgap : n;

		/*
		 * Make sure we don't overflow the buffer
		 */

		if ((rc + piece) > TTY_FLIPBUF_SIZE)
			piece = TTY_FLIPBUF_SIZE - rc;

		if (piece == 0)
			break;

		memcpy(rptr, ch->rxptr + tail, piece);
		rptr += piece;
		rc += piece;
		tail = (tail + piece) & wrapmask;
		n -= piece;
	}
	tty->flip.count = rc;
	tty->flip.char_buf_ptr = rptr;
    globalwinon(ch);
	bc->rout = tail;

	/* Must be called with global data */
	tty_schedule_flip(ch->tty); 
	return;
}


static int pcxe_tiocmget(struct tty_struct *tty, struct file *file)
{
	struct channel *ch = (struct channel *) tty->driver_data;
	volatile struct board_chan *bc;
	unsigned long flags;
	int mflag = 0;
	int mstat;

	if(ch)
		bc = ch->brdchan;
	else {
		printk("ch is NULL in %s!\n", __FUNCTION__);
		return(-EINVAL);
	}

	save_flags(flags);
	cli();
	globalwinon(ch);
	mstat = bc->mstat;
	memoff(ch);
	restore_flags(flags);

	if(mstat & DTR)
		mflag |= TIOCM_DTR;
	if(mstat & RTS)
		mflag |= TIOCM_RTS;
	if(mstat & CTS)
		mflag |= TIOCM_CTS;
	if(mstat & ch->dsr)
		mflag |= TIOCM_DSR;
	if(mstat & RI)
		mflag |= TIOCM_RI;
	if(mstat & ch->dcd)
		mflag |= TIOCM_CD;

	return mflag;
}


static int pcxe_tiocmset(struct tty_struct *tty, struct file *file,
			 unsigned int set, unsigned int clear)
{
	struct channel *ch = (struct channel *) tty->driver_data;
	volatile struct board_chan *bc;
	unsigned long flags;

	if(ch)
		bc = ch->brdchan;
	else {
		printk("ch is NULL in %s!\n", __FUNCTION__);
		return(-EINVAL);
	}

	save_flags(flags);
	cli();
	/*
	 * I think this modemfake stuff is broken.  It doesn't
	 * correctly reflect the behaviour desired by the TIOCM*
	 * ioctls.  Therefore this is probably broken.
	 */
	if (set & TIOCM_DTR) {
		ch->modemfake |= DTR;
		ch->modem |= DTR;
	}
	if (set & TIOCM_RTS) {
		ch->modemfake |= RTS;
		ch->modem |= RTS;
	}

	if (clear & TIOCM_DTR) {
		ch->modemfake |= DTR;
		ch->modem &= ~DTR;
	}
	if (clear & TIOCM_RTS) {
		ch->modemfake |= RTS;
		ch->modem &= ~RTS;
	}
	globalwinon(ch);
	pcxxparam(tty,ch);
	memoff(ch);
	restore_flags(flags);
	return 0;
}


static int pcxe_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	struct channel *ch = (struct channel *) tty->driver_data;
	volatile struct board_chan *bc;
	int retval;
	unsigned int mflag, mstat;
	unsigned char startc, stopc;
	unsigned long flags;
	digiflow_t dflow;

	if(ch)
		bc = ch->brdchan;
	else {
		printk("ch is NULL in pcxe_ioctl!\n");
		return(-EINVAL);
	}

	save_flags(flags);

	switch(cmd) {
		case TCSBRK:	/* SVID version: non-zero arg --> no break */
			retval = tty_check_change(tty);
			if(retval)
				return retval;
			setup_empty_event(tty,ch);		
			tty_wait_until_sent(tty, 0);
			if(!arg)
				digi_send_break(ch, HZ/4);    /* 1/4 second */
			return 0;

		case TCSBRKP:	/* support for POSIX tcsendbreak() */
			retval = tty_check_change(tty);
			if(retval)
				return retval;
			setup_empty_event(tty,ch);		
			tty_wait_until_sent(tty, 0);
			digi_send_break(ch, arg ? arg*(HZ/10) : HZ/4);
			return 0;

		case TIOCGSOFTCAR:
			return put_user(C_CLOCAL(tty) ? 1 : 0, (unsigned int *)arg);

		case TIOCSSOFTCAR:
			{
			    unsigned int value;
			    if (get_user(value, (unsigned int *) arg))
				    return -EFAULT;
			    tty->termios->c_cflag = ((tty->termios->c_cflag & ~CLOCAL) | (value ? CLOCAL : 0));
			}
			return 0;

		case TIOCMODG:
			mflag = pcxe_tiocmget(tty, file);
			if (put_user(mflag, (unsigned int *) arg))
				return -EFAULT;
			break;

		case TIOCMODS:
			if (get_user(mstat, (unsigned int *) arg))
				return -EFAULT;
			return pcxe_tiocmset(tty, file, mstat, ~mstat);

		case TIOCSDTR:
			cli();
			ch->omodem |= DTR;
			globalwinon(ch);
			fepcmd(ch, SETMODEM, DTR, 0, 10, 1);
			memoff(ch);
			restore_flags(flags);
			break;

		case TIOCCDTR:
			ch->omodem &= ~DTR;
			cli();
			globalwinon(ch);
			fepcmd(ch, SETMODEM, 0, DTR, 10, 1);
			memoff(ch);
			restore_flags(flags);
			break;

		case DIGI_GETA:
			if (copy_to_user((char*)arg, &ch->digiext, sizeof(digi_t)))
				return -EFAULT;
			break;

		case DIGI_SETAW:
		case DIGI_SETAF:
			if(cmd == DIGI_SETAW) {
				setup_empty_event(tty,ch);		
				tty_wait_until_sent(tty, 0);
			}
			else {
				tty_ldisc_flush(tty);
			}

			/* Fall Thru */

		case DIGI_SETA:
			if (copy_from_user(&ch->digiext, (char*)arg, sizeof(digi_t)))
				return -EFAULT;
#ifdef DEBUG_IOCTL
			printk("ioctl(DIGI_SETA): flags = %x\n", ch->digiext.digi_flags);
#endif
			
			if(ch->digiext.digi_flags & DIGI_ALTPIN) {
				ch->dcd = DSR;
				ch->dsr = CD;
			} else {
				ch->dcd = CD;
				ch->dsr = DSR;
			}
		
			cli();
			globalwinon(ch);
			pcxxparam(tty,ch);
			memoff(ch);
			restore_flags(flags);
			break;

		case DIGI_GETFLOW:
		case DIGI_GETAFLOW:
			cli();	
			globalwinon(ch);
			if(cmd == DIGI_GETFLOW) {
				dflow.startc = bc->startc;
				dflow.stopc = bc->stopc;
			} else {
				dflow.startc = bc->startca;
				dflow.stopc = bc->stopca;
			}
			memoff(ch);
			restore_flags(flags);

			if (copy_to_user((char*)arg, &dflow, sizeof(dflow)))
				return -EFAULT;
			break;

		case DIGI_SETAFLOW:
		case DIGI_SETFLOW:
			if(cmd == DIGI_SETFLOW) {
				startc = ch->startc;
				stopc = ch->stopc;
			} else {
				startc = ch->startca;
				stopc = ch->stopca;
			}

			if (copy_from_user(&dflow, (char*)arg, sizeof(dflow)))
				return -EFAULT;

			if(dflow.startc != startc || dflow.stopc != stopc) {
				cli();
				globalwinon(ch);

				if(cmd == DIGI_SETFLOW) {
					ch->fepstartc = ch->startc = dflow.startc;
					ch->fepstopc = ch->stopc = dflow.stopc;
					fepcmd(ch,SONOFFC,ch->fepstartc,ch->fepstopc,0, 1);
				} else {
					ch->fepstartca = ch->startca = dflow.startc;
					ch->fepstopca  = ch->stopca = dflow.stopc;
					fepcmd(ch, SAUXONOFFC, ch->fepstartca, ch->fepstopca, 0, 1);
				}

				if(ch->statusflags & TXSTOPPED)
					pcxe_start(tty);

				memoff(ch);
				restore_flags(flags);
			}
			break;

		default:
			return -ENOIOCTLCMD;
	}

	return 0;
}

static void pcxe_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct channel *info;

	if ((info=chan(tty))!=NULL) {
		unsigned long flags;
		save_flags(flags);
		cli();
		globalwinon(info);
		pcxxparam(tty,info);
		memoff(info);

		if ((old_termios->c_cflag & CRTSCTS) &&
			((tty->termios->c_cflag & CRTSCTS) == 0))
			tty->hw_stopped = 0;
		if(!(old_termios->c_cflag & CLOCAL) &&
			(tty->termios->c_cflag & CLOCAL))
			wake_up_interruptible(&info->open_wait);
		restore_flags(flags);
	}
}



static void do_softint(void *private_)
{
	struct channel *info = (struct channel *) private_;
	
	if(info && info->magic == PCXX_MAGIC) {
		struct tty_struct *tty = info->tty;
		if (tty && tty->driver_data) {
			if(test_and_clear_bit(PCXE_EVENT_HANGUP, &info->event)) {
				tty_hangup(tty);
				wake_up_interruptible(&info->open_wait);
				info->asyncflags &= ~ASYNC_NORMAL_ACTIVE;
			}
		}
	}
}


static void pcxe_stop(struct tty_struct *tty)
{
	struct channel *info;

	if ((info=chan(tty))!=NULL) {
		unsigned long flags;
		save_flags(flags); 
		cli();
		if ((info->statusflags & TXSTOPPED) == 0) {
			globalwinon(info);
			fepcmd(info, PAUSETX, 0, 0, 0, 0);
			info->statusflags |= TXSTOPPED;
			memoff(info);
		}
		restore_flags(flags);
	}
}

static void pcxe_throttle(struct tty_struct * tty)
{
	struct channel *info;

	if ((info=chan(tty))!=NULL) {
		unsigned long flags;
		save_flags(flags);
		cli();
		if ((info->statusflags & RXSTOPPED) == 0) {
			globalwinon(info);
			fepcmd(info, PAUSERX, 0, 0, 0, 0);
			info->statusflags |= RXSTOPPED;
			memoff(info);
		}
		restore_flags(flags);
	}
}

static void pcxe_unthrottle(struct tty_struct *tty)
{
	struct channel *info;

	if ((info=chan(tty)) != NULL) {
		unsigned long flags;

		/* Just in case output was resumed because of a change in Digi-flow */
		save_flags(flags);
		cli();
		if(info->statusflags & RXSTOPPED) {
			volatile struct board_chan *bc;
			globalwinon(info);
			bc = info->brdchan;
			fepcmd(info, RESUMERX, 0, 0, 0, 0);
			info->statusflags &= ~RXSTOPPED;
			memoff(info);
		}
		restore_flags(flags);
	}
}


static void pcxe_start(struct tty_struct *tty)
{
	struct channel *info;

	if ((info=chan(tty))!=NULL) {
		unsigned long flags;

		save_flags(flags);
		cli();
		/* Just in case output was resumed because of a change in Digi-flow */
		if(info->statusflags & TXSTOPPED) {
			volatile struct board_chan *bc;
			globalwinon(info);
			bc = info->brdchan;
			if(info->statusflags & LOWWAIT)
				bc->ilow = 1;
			fepcmd(info, RESUMETX, 0, 0, 0, 0);
			info->statusflags &= ~TXSTOPPED;
			memoff(info);
		}
		restore_flags(flags);
	}
}


void digi_send_break(struct channel *ch, int msec)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	globalwinon(ch);

	/* 
	 * Maybe I should send an infinite break here, schedule() for
	 * msec amount of time, and then stop the break.  This way,
	 * the user can't screw up the FEP by causing digi_send_break()
	 * to be called (i.e. via an ioctl()) more than once in msec amount 
	 * of time.  Try this for now...
	 */

	fepcmd(ch, SENDBREAK, msec, 0, 10, 0);
	memoff(ch);

	restore_flags(flags);
}

static void setup_empty_event(struct tty_struct *tty, struct channel *ch)
{
	volatile struct board_chan *bc;
	unsigned long flags;

	save_flags(flags);
	cli();
	globalwinon(ch);
	ch->statusflags |= EMPTYWAIT;
	bc = ch->brdchan;
	bc->iempty = 1;
	memoff(ch);
	restore_flags(flags);
}
