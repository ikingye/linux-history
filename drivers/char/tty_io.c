/*
 *  linux/drivers/char/tty_io.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * 'tty_io.c' gives an orthogonal feeling to tty's, be they consoles
 * or rs-channels. It also implements echoing, cooked mode etc.
 *
 * Kill-line thanks to John T Kohl, who also corrected VMIN = VTIME = 0.
 *
 * Modified by Theodore Ts'o, 9/14/92, to dynamically allocate the
 * tty_struct and tty_queue structures.  Previously there was an array
 * of 256 tty_struct's which was statically allocated, and the
 * tty_queue structures were allocated at boot time.  Both are now
 * dynamically allocated only when the tty is open.
 *
 * Also restructured routines so that there is more of a separation
 * between the high-level tty routines (tty_io.c and tty_ioctl.c) and
 * the low-level tty routines (serial.c, pty.c, console.c).  This
 * makes for cleaner and more compact code.  -TYT, 9/17/92 
 *
 * Modified by Fred N. van Kempen, 01/29/93, to add line disciplines
 * which can be dynamically activated and de-activated by the line
 * discipline handling modules (like SLIP).
 *
 * NOTE: pay no attention to the line discipline code (yet); its
 * interface is still subject to change in this version...
 * -- TYT, 1/31/92
 *
 * Added functionality to the OPOST tty handling.  No delays, but all
 * other bits should be there.
 *	-- Nick Holloway <alfie@dcs.warwick.ac.uk>, 27th May 1993.
 *
 * Rewrote canonical mode and added more termios flags.
 * 	-- julian@uhunix.uhcc.hawaii.edu (J. Cowley), 13Jan94
 *
 * Reorganized FASYNC support so mouse code can share it.
 *	-- ctm@ardi.com, 9Sep95
 *
 * New TIOCLINUX variants added.
 *	-- mj@k332.feld.cvut.cz, 19-Nov-95
 * 
 * Restrict vt switching via ioctl()
 *      -- grif@cs.ucr.edu, 5-Dec-95
 *
 * Move console and virtual terminal code to more apropriate files,
 * implement CONFIG_VT and generalize console device interface.
 *	-- Marko Kohtala <Marko.Kohtala@hut.fi>, March 97
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/console.h>
#include <linux/timer.h>
#include <linux/ctype.h>
#include <linux/kd.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/poll.h>
#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#endif
#include <linux/init.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include <linux/kbd_kern.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>

#ifdef CONFIG_KERNELD
#include <linux/kerneld.h>
#endif

#define CONSOLE_DEV MKDEV(TTY_MAJOR,0)
#define TTY_DEV MKDEV(TTYAUX_MAJOR,0)

#undef TTY_DEBUG_HANGUP

#define TTY_PARANOIA_CHECK
#define CHECK_TTY_COUNT

struct termios tty_std_termios;		/* for the benefit of tty drivers  */
struct tty_driver *tty_drivers = NULL;	/* linked list of tty drivers */
struct tty_ldisc ldiscs[NR_LDISCS];	/* line disc dispatch table	*/

/*
 * redirect is the pseudo-tty that console output
 * is redirected to if asked by TIOCCONS.
 */
struct tty_struct * redirect = NULL;

static void initialize_tty_struct(struct tty_struct *tty);

static long tty_read(struct inode *, struct file *, char *, unsigned long);
static long tty_write(struct inode *, struct file *, const char *, unsigned long);
static unsigned int tty_poll(struct file *, poll_table *);
static int tty_open(struct inode *, struct file *);
static int tty_release(struct inode *, struct file *);
static int tty_ioctl(struct inode * inode, struct file * file,
		     unsigned int cmd, unsigned long arg);
static int tty_fasync(struct inode * inode, struct file * filp, int on);

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

/*
 * These two routines return the name of tty.  tty_name() should NOT
 * be used in interrupt drivers, since it's not re-entrant.  Use
 * _tty_name() instead.
 */
char *_tty_name(struct tty_struct *tty, char *buf)
{
	if (tty)
		sprintf(buf, "%s%d", tty->driver.name,
			MINOR(tty->device) - tty->driver.minor_start +
			tty->driver.name_base);
	else
		strcpy(buf, "NULL tty");
	return buf;
}

char *tty_name(struct tty_struct *tty)
{
	static char buf[64];

	return(_tty_name(tty, buf));
}

inline int tty_paranoia_check(struct tty_struct *tty, kdev_t device,
			      const char *routine)
{
#ifdef TTY_PARANOIA_CHECK
	static const char *badmagic =
		"Warning: bad magic number for tty struct (%s) in %s\n";
	static const char *badtty =
		"Warning: null TTY for (%s) in %s\n";

	if (!tty) {
		printk(badtty, kdevname(device), routine);
		return 1;
	}
	if (tty->magic != TTY_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

static int check_tty_count(struct tty_struct *tty, const char *routine)
{
#ifdef CHECK_TTY_COUNT
	struct file *f;
	int count = 0;
	
	for(f = inuse_filps; f; f = f->f_next) {
		if(f->private_data == tty)
			count++;
	}
	if (tty->driver.type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver.subtype == PTY_TYPE_SLAVE &&
	    tty->link && tty->link->count)
		count++;
	if (tty->count != count) {
		printk("Warning: dev (%s) tty->count(%d) != #fd's(%d) in %s\n",
		       kdevname(tty->device), tty->count, count, routine);
		return count;
       }	
#endif
	return 0;
}

int tty_register_ldisc(int disc, struct tty_ldisc *new_ldisc)
{
	if (disc < N_TTY || disc >= NR_LDISCS)
		return -EINVAL;
	
	if (new_ldisc) {
		ldiscs[disc] = *new_ldisc;
		ldiscs[disc].flags |= LDISC_FLAG_DEFINED;
		ldiscs[disc].num = disc;
	} else
		memset(&ldiscs[disc], 0, sizeof(struct tty_ldisc));
	
	return 0;
}

/* Set the discipline of a tty line. */
static int tty_set_ldisc(struct tty_struct *tty, int ldisc)
{
	int	retval = 0;
	struct	tty_ldisc o_ldisc;

	if ((ldisc < N_TTY) || (ldisc >= NR_LDISCS))
		return -EINVAL;
#ifdef CONFIG_KERNELD
	/* Eduardo Blanco <ejbs@cs.cs.com.uy> */
	if (!(ldiscs[ldisc].flags & LDISC_FLAG_DEFINED)) {
		char modname [20];
		sprintf(modname, "tty-ldisc-%d", ldisc);
		request_module (modname);
	}
#endif
	if (!(ldiscs[ldisc].flags & LDISC_FLAG_DEFINED))
		return -EINVAL;

	if (tty->ldisc.num == ldisc)
		return 0;	/* We are already in the desired discipline */
	o_ldisc = tty->ldisc;

	tty_wait_until_sent(tty, 0);
	
	/* Shutdown the current discipline. */
	if (tty->ldisc.close)
		(tty->ldisc.close)(tty);

	/* Now set up the new line discipline. */
	tty->ldisc = ldiscs[ldisc];
	tty->termios->c_line = ldisc;
	if (tty->ldisc.open)
		retval = (tty->ldisc.open)(tty);
	if (retval < 0) {
		tty->ldisc = o_ldisc;
		tty->termios->c_line = tty->ldisc.num;
		if (tty->ldisc.open && (tty->ldisc.open(tty) < 0)) {
			tty->ldisc = ldiscs[N_TTY];
			tty->termios->c_line = N_TTY;
			if (tty->ldisc.open) {
				int r = tty->ldisc.open(tty);

				if (r < 0)
					panic("Couldn't open N_TTY ldisc for "
					      "%s --- error %d.",
					      tty_name(tty), r);
			}
		}
	}
	if (tty->ldisc.num != o_ldisc.num && tty->driver.set_ldisc)
		tty->driver.set_ldisc(tty);
	return retval;
}

/*
 * This routine returns a tty driver structure, given a device number
 */
struct tty_driver *get_tty_driver(kdev_t device)
{
	int	major, minor;
	struct tty_driver *p;
	
	minor = MINOR(device);
	major = MAJOR(device);

	for (p = tty_drivers; p; p = p->next) {
		if (p->major != major)
			continue;
		if (minor < p->minor_start)
			continue;
		if (minor >= p->minor_start + p->num)
			continue;
		return p;
	}
	return NULL;
}

/*
 * If we try to write to, or set the state of, a terminal and we're
 * not in the foreground, send a SIGTTOU.  If the signal is blocked or
 * ignored, go ahead and perform the operation.  (POSIX 7.2)
 */
int tty_check_change(struct tty_struct * tty)
{
	if (current->tty != tty)
		return 0;
	if (tty->pgrp <= 0) {
		printk("tty_check_change: tty->pgrp <= 0!\n");
		return 0;
	}
	if (current->pgrp == tty->pgrp)
		return 0;
	if (is_ignored(SIGTTOU))
		return 0;
	if (is_orphaned_pgrp(current->pgrp))
		return -EIO;
	(void) kill_pg(current->pgrp,SIGTTOU,1);
	return -ERESTARTSYS;
}

static long hung_up_tty_read(struct inode * inode, struct file * file,
	char * buf, unsigned long count)
{
	return 0;
}

static long hung_up_tty_write(struct inode * inode,
	struct file * file, const char * buf, unsigned long count)
{
	return -EIO;
}

static unsigned int hung_up_tty_poll(struct file * filp, poll_table * wait)
{
	return POLLIN | POLLOUT | POLLERR | POLLHUP | POLLRDNORM | POLLWRNORM;
}

static int hung_up_tty_ioctl(struct inode * inode, struct file * file,
			     unsigned int cmd, unsigned long arg)
{
	return cmd == TIOCSPGRP ? -ENOTTY : -EIO;
}

static long long tty_lseek(struct inode * inode, struct file * file,
	long long offset, int orig)
{
	return -ESPIPE;
}

static struct file_operations tty_fops = {
	tty_lseek,
	tty_read,
	tty_write,
	NULL,		/* tty_readdir */
	tty_poll,
	tty_ioctl,
	NULL,		/* tty_mmap */
	tty_open,
	tty_release,
	NULL,		/* tty_fsync */
	tty_fasync
};

static struct file_operations hung_up_tty_fops = {
	tty_lseek,
	hung_up_tty_read,
	hung_up_tty_write,
	NULL,		/* hung_up_tty_readdir */
	hung_up_tty_poll,
	hung_up_tty_ioctl,
	NULL,		/* hung_up_tty_mmap */
	NULL,		/* hung_up_tty_open */
	tty_release,	/* hung_up_tty_release */
	NULL,		/* hung_up_tty_fsync  */
	NULL		/* hung_up_tty_fasync */
};

void do_tty_hangup(struct tty_struct * tty, struct file_operations *fops)
{

	struct file * filp;
	struct task_struct *p;

	if (!tty)
		return;
	check_tty_count(tty, "do_tty_hangup");
	for (filp = inuse_filps; filp; filp = filp->f_next) {
		if (filp->private_data != tty)
			continue;
		if (!filp->f_inode)
			continue;
		if (filp->f_inode->i_rdev == CONSOLE_DEV)
			continue;
		if (filp->f_op != &tty_fops)
			continue;
		tty_fasync(filp->f_inode, filp, 0);
		filp->f_op = fops;
	}
	
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
	wake_up_interruptible(&tty->write_wait);
	wake_up_interruptible(&tty->read_wait);

	/*
	 * Shutdown the current line discipline, and reset it to
	 * N_TTY.
	 */
	if (tty->ldisc.num != ldiscs[N_TTY].num) {
		if (tty->ldisc.close)
			(tty->ldisc.close)(tty);
		tty->ldisc = ldiscs[N_TTY];
		tty->termios->c_line = N_TTY;
		if (tty->ldisc.open) {
			int i = (tty->ldisc.open)(tty);
			if (i < 0)
				printk("do_tty_hangup: N_TTY open: error %d\n",
				       -i);
		}
	}
	
	read_lock(&tasklist_lock);
 	for_each_task(p) {
		if ((tty->session > 0) && (p->session == tty->session) &&
		    p->leader) {
			send_sig(SIGHUP,p,1);
			send_sig(SIGCONT,p,1);
			if (tty->pgrp > 0)
				p->tty_old_pgrp = tty->pgrp;
		}
		if (p->tty == tty)
			p->tty = NULL;
	}
	read_unlock(&tasklist_lock);

	tty->flags = 0;
	tty->session = 0;
	tty->pgrp = -1;
	tty->ctrl_status = 0;
	if (tty->driver.flags & TTY_DRIVER_RESET_TERMIOS)
		*tty->termios = tty->driver.init_termios;
	if (tty->driver.hangup)
		(tty->driver.hangup)(tty);
}

void tty_hangup(struct tty_struct * tty)
{
#ifdef TTY_DEBUG_HANGUP
	printk("%s hangup...\n", tty_name(tty));
#endif
	do_tty_hangup(tty, &hung_up_tty_fops);
}

void tty_vhangup(struct tty_struct * tty)
{
#ifdef TTY_DEBUG_HANGUP
	printk("%s vhangup...\n", tty_name(tty));
#endif
	do_tty_hangup(tty, &hung_up_tty_fops);
}

int tty_hung_up_p(struct file * filp)
{
	return (filp->f_op == &hung_up_tty_fops);
}

/*
 * This function is typically called only by the session leader, when
 * it wants to disassociate itself from its controlling tty.
 *
 * It performs the following functions:
 * 	(1)  Sends a SIGHUP and SIGCONT to the foreground process group
 * 	(2)  Clears the tty from being controlling the session
 * 	(3)  Clears the controlling tty for all processes in the
 * 		session group.
 *
 * The argument on_exit is set to 1 if called when a process is
 * exiting; it is 0 if called by the ioctl TIOCNOTTY.
 */
void disassociate_ctty(int on_exit)
{
	struct tty_struct *tty = current->tty;
	struct task_struct *p;
	int tty_pgrp = -1;

	if (tty) {
		tty_pgrp = tty->pgrp;
		if (on_exit && tty->driver.type != TTY_DRIVER_TYPE_PTY)
			tty_vhangup(tty);
	} else {
		if (current->tty_old_pgrp) {
			kill_pg(current->tty_old_pgrp, SIGHUP, on_exit);
			kill_pg(current->tty_old_pgrp, SIGCONT, on_exit);
		}
		return;
	}
	if (tty_pgrp > 0) {
		kill_pg(tty_pgrp, SIGHUP, on_exit);
		if (!on_exit)
			kill_pg(tty_pgrp, SIGCONT, on_exit);
	}

	current->tty_old_pgrp = 0;
	tty->session = 0;
	tty->pgrp = -1;

	read_lock(&tasklist_lock);
	for_each_task(p)
	  	if (p->session == current->session)
			p->tty = NULL;
	read_unlock(&tasklist_lock);
}

void wait_for_keypress(void)
{
        struct console *c = console_drivers;
        while(c && !c->wait_key)
                c = c->next;
        if (c) c->wait_key();
}

void stop_tty(struct tty_struct *tty)
{
	if (tty->stopped)
		return;
	tty->stopped = 1;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_START;
		tty->ctrl_status |= TIOCPKT_STOP;
		wake_up_interruptible(&tty->link->read_wait);
	}
	if (tty->driver.stop)
		(tty->driver.stop)(tty);
}

void start_tty(struct tty_struct *tty)
{
	if (!tty->stopped || tty->flow_stopped)
		return;
	tty->stopped = 0;
	if (tty->link && tty->link->packet) {
		tty->ctrl_status &= ~TIOCPKT_STOP;
		tty->ctrl_status |= TIOCPKT_START;
		wake_up_interruptible(&tty->link->read_wait);
	}
	if (tty->driver.start)
		(tty->driver.start)(tty);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
	wake_up_interruptible(&tty->write_wait);
}

static long tty_read(struct inode * inode, struct file * file,
	char * buf, unsigned long count)
{
	int i;
	struct tty_struct * tty;

	tty = (struct tty_struct *)file->private_data;
	if (tty_paranoia_check(tty, inode->i_rdev, "tty_read"))
		return -EIO;
	if (!tty || (tty->flags & (1 << TTY_IO_ERROR)))
		return -EIO;

	/* This check not only needs to be done before reading, but also
	   whenever read_chan() gets woken up after sleeping, so I've
	   moved it to there.  This should only be done for the N_TTY
	   line discipline, anyway.  Same goes for write_chan(). -- jlc. */
#if 0
	if ((inode->i_rdev != CONSOLE_DEV) && /* don't stop on /dev/console */
	    (tty->pgrp > 0) &&
	    (current->tty == tty) &&
	    (tty->pgrp != current->pgrp))
		if (is_ignored(SIGTTIN) || is_orphaned_pgrp(current->pgrp))
			return -EIO;
		else {
			(void) kill_pg(current->pgrp, SIGTTIN, 1);
			return -ERESTARTSYS;
		}
#endif
	if (tty->ldisc.read)
		i = (tty->ldisc.read)(tty,file,buf,count);
	else
		i = -EIO;
	if (i > 0)
		inode->i_atime = CURRENT_TIME;
	return i;
}

/*
 * Split writes up in sane blocksizes to avoid
 * denial-of-service type attacks
 */
static inline int do_tty_write(
	int (*write)(struct tty_struct *, struct file *, const unsigned char *, unsigned int),
	struct inode *inode,
	struct tty_struct *tty,
	struct file *file,
	const unsigned char *buf,
	unsigned int count)
{
	int ret = 0, written = 0;

	for (;;) {
		unsigned long size = PAGE_SIZE*2;
		if (size > count)
			size = count;
		ret = write(tty, file, buf, size);
		if (ret <= 0)
			break;
		written += ret;
		buf += ret;
		count -= ret;
		if (!count)
			break;
		ret = -ERESTARTSYS;
		if (current->signal & ~current->blocked)
			break;
		if (need_resched)
			schedule();
	}
	if (written) {
		inode->i_mtime = CURRENT_TIME;
		ret = written;
	}
	return ret;
}


static long tty_write(struct inode * inode, struct file * file,
	const char * buf, unsigned long count)
{
	int is_console;
	struct tty_struct * tty;

	is_console = (inode->i_rdev == CONSOLE_DEV);

	if (is_console && redirect)
		tty = redirect;
	else
		tty = (struct tty_struct *)file->private_data;
	if (tty_paranoia_check(tty, inode->i_rdev, "tty_write"))
		return -EIO;
	if (!tty || !tty->driver.write || (tty->flags & (1 << TTY_IO_ERROR)))
		return -EIO;
#if 0
	if (!is_console && L_TOSTOP(tty) && (tty->pgrp > 0) &&
	    (current->tty == tty) && (tty->pgrp != current->pgrp)) {
		if (is_orphaned_pgrp(current->pgrp))
			return -EIO;
		if (!is_ignored(SIGTTOU)) {
			(void) kill_pg(current->pgrp, SIGTTOU, 1);
			return -ERESTARTSYS;
		}
	}
#endif
	if (!tty->ldisc.write)
		return -EIO;
	return do_tty_write(tty->ldisc.write,
		inode, tty, file,
		(const unsigned char *)buf,
		(unsigned int)count);
}

/*
 * This is so ripe with races that you should *really* not touch this
 * unless you know exactly what you are doing. All the changes have to be
 * made atomically, or there may be incorrect pointers all over the place.
 */
static int init_dev(kdev_t device, struct tty_struct **ret_tty)
{
	struct tty_struct *tty, **tty_loc, *o_tty, **o_tty_loc;
	struct termios *tp, **tp_loc, *o_tp, **o_tp_loc;
	struct termios *ltp, **ltp_loc, *o_ltp, **o_ltp_loc;
	struct tty_driver *driver;	
	int retval;
	int idx;

	driver = get_tty_driver(device);
	if (!driver)
		return -ENODEV;

	idx = MINOR(device) - driver->minor_start;
	tty = o_tty = NULL;
	tp = o_tp = NULL;
	ltp = o_ltp = NULL;
	o_tty_loc = NULL;
	o_tp_loc = o_ltp_loc = NULL;

	tty_loc = &driver->table[idx];
	tp_loc = &driver->termios[idx];
	ltp_loc = &driver->termios_locked[idx];

repeat:
	retval = -EIO;
	if (driver->type == TTY_DRIVER_TYPE_PTY &&
	    driver->subtype == PTY_TYPE_MASTER &&
	    *tty_loc && (*tty_loc)->count)
		goto end_init;
	retval = -ENOMEM;
	if (!*tty_loc && !tty) {
		if (!(tty = (struct tty_struct*) get_free_page(GFP_KERNEL)))
			goto end_init;
		initialize_tty_struct(tty);
		tty->device = device;
		tty->driver = *driver;
		goto repeat;
	}
	if (!*tp_loc && !tp) {
		tp = (struct termios *) kmalloc(sizeof(struct termios),
						GFP_KERNEL);
		if (!tp)
			goto end_init;
		*tp = driver->init_termios;
		goto repeat;
	}
	if (!*ltp_loc && !ltp) {
		ltp = (struct termios *) kmalloc(sizeof(struct termios),
						 GFP_KERNEL);
		if (!ltp)
			goto end_init;
		memset(ltp, 0, sizeof(struct termios));
		goto repeat;
	}
	if (driver->type == TTY_DRIVER_TYPE_PTY) {
		o_tty_loc = &driver->other->table[idx];
		o_tp_loc = &driver->other->termios[idx];
		o_ltp_loc = &driver->other->termios_locked[idx];

		if (!*o_tty_loc && !o_tty) {
			kdev_t 	o_device;
			
			o_tty = (struct tty_struct *)
				get_free_page(GFP_KERNEL);
			if (!o_tty)
				goto end_init;
			o_device = MKDEV(driver->other->major,
					 driver->other->minor_start + idx);
			initialize_tty_struct(o_tty);
			o_tty->device = o_device;
			o_tty->driver = *driver->other;
			goto repeat;
		}
		if (!*o_tp_loc && !o_tp) {
			o_tp = (struct termios *)
				kmalloc(sizeof(struct termios), GFP_KERNEL);
			if (!o_tp)
				goto end_init;
			*o_tp = driver->other->init_termios;
			goto repeat;
		}
		if (!*o_ltp_loc && !o_ltp) {
			o_ltp = (struct termios *)
				kmalloc(sizeof(struct termios), GFP_KERNEL);
			if (!o_ltp)
				goto end_init;
			memset(o_ltp, 0, sizeof(struct termios));
			goto repeat;
		}
		
	}
	/* Now we have allocated all the structures: update all the pointers.. */
	if (!*tp_loc) {
		*tp_loc = tp;
		tp = NULL;
	}
	if (!*ltp_loc) {
		*ltp_loc = ltp;
		ltp = NULL;
	}
	if (!*tty_loc) {
		tty->termios = *tp_loc;
		tty->termios_locked = *ltp_loc;
		*tty_loc = tty;
		(*driver->refcount)++;
		(*tty_loc)->count++;
		if (tty->ldisc.open) {
			retval = (tty->ldisc.open)(tty);
			if (retval < 0) {
				(*tty_loc)->count--;
				tty = NULL;
				goto end_init;
			}
		}
		tty = NULL;
	} else {
		if ((*tty_loc)->flags & (1 << TTY_CLOSING)) {
			printk("Attempt to open closing tty %s.\n",
			       tty_name(*tty_loc));
			printk("Ack!!!!  This should never happen!!\n");
			return -EINVAL;
		}
		(*tty_loc)->count++;
	}
	if (driver->type == TTY_DRIVER_TYPE_PTY) {
		if (!*o_tp_loc) {
			*o_tp_loc = o_tp;
			o_tp = NULL;
		}
		if (!*o_ltp_loc) {
			*o_ltp_loc = o_ltp;
			o_ltp = NULL;
		}
		if (!*o_tty_loc) {
			o_tty->termios = *o_tp_loc;
			o_tty->termios_locked = *o_ltp_loc;
			*o_tty_loc = o_tty;
			(*driver->other->refcount)++;
			if (o_tty->ldisc.open) {
				retval = (o_tty->ldisc.open)(o_tty);
				if (retval < 0) {
					(*tty_loc)->count--;
					o_tty = NULL;
					goto end_init;
				}
			}
			o_tty = NULL;
		}
		(*tty_loc)->link = *o_tty_loc;
		(*o_tty_loc)->link = *tty_loc;
		if (driver->subtype == PTY_TYPE_MASTER)
			(*o_tty_loc)->count++;
	}
	(*tty_loc)->driver = *driver;
	*ret_tty = *tty_loc;
	retval = 0;
end_init:
	if (tty)
		free_page((unsigned long) tty);
	if (o_tty)
		free_page((unsigned long) o_tty);
	if (tp)
		kfree_s(tp, sizeof(struct termios));
	if (o_tp)
		kfree_s(o_tp, sizeof(struct termios));
	if (ltp)
		kfree_s(ltp, sizeof(struct termios));
	if (o_ltp)
		kfree_s(o_ltp, sizeof(struct termios));
	return retval;
}

/*
 * Even releasing the tty structures is a tricky business.. We have
 * to be very careful that the structures are all released at the
 * same time, as interrupts might otherwise get the wrong pointers.
 */
static void release_dev(struct file * filp)
{
	struct tty_struct *tty, *o_tty;
	struct termios *tp, *o_tp, *ltp, *o_ltp;
	struct task_struct *p;
	int	idx;
	
	tty = (struct tty_struct *)filp->private_data;
	if (tty_paranoia_check(tty, filp->f_inode->i_rdev, "release_dev"))
		return;

	check_tty_count(tty, "release_dev");

	tty_fasync(filp->f_inode, filp, 0);

	tp = tty->termios;
	ltp = tty->termios_locked;

	idx = MINOR(tty->device) - tty->driver.minor_start;
#ifdef TTY_PARANOIA_CHECK
	if (idx < 0 || idx >= tty->driver.num) {
		printk("release_dev: bad idx when trying to free (%s)\n",
		       kdevname(tty->device));
		return;
	}
	if (tty != tty->driver.table[idx]) {
		printk("release_dev: driver.table[%d] not tty for (%s)\n",
		       idx, kdevname(tty->device));
		return;
	}
	if (tp != tty->driver.termios[idx]) {
		printk("release_dev: driver.termios[%d] not termios for ("
		       "%s)\n",
		       idx, kdevname(tty->device));
		return;
	}
	if (ltp != tty->driver.termios_locked[idx]) {
		printk("release_dev: driver.termios_locked[%d] not termios_locked for ("
		       "%s)\n",
		       idx, kdevname(tty->device));
		return;
	}
#endif

#ifdef TTY_DEBUG_HANGUP
	printk("release_dev of %s (tty count=%d)...", tty_name(tty),
	       tty->count);
#endif

	o_tty = tty->link;
	o_tp = (o_tty) ? o_tty->termios : NULL;
	o_ltp = (o_tty) ? o_tty->termios_locked : NULL;

#ifdef TTY_PARANOIA_CHECK
	if (tty->driver.other) {
		if (o_tty != tty->driver.other->table[idx]) {
			printk("release_dev: other->table[%d] not o_tty for ("
			       "%s)\n",
			       idx, kdevname(tty->device));
			return;
		}
		if (o_tp != tty->driver.other->termios[idx]) {
			printk("release_dev: other->termios[%d] not o_termios for ("
			       "%s)\n",
			       idx, kdevname(tty->device));
			return;
		}
		if (o_ltp != tty->driver.other->termios_locked[idx]) {
			printk("release_dev: other->termios_locked[%d] not o_termios_locked for ("
			       "%s)\n",
			       idx, kdevname(tty->device));
			return;
		}

		if (o_tty->link != tty) {
			printk("release_dev: bad pty pointers\n");
			return;
		}
	}
#endif
	
	if (tty->driver.close)
		tty->driver.close(tty, filp);
	if (tty->driver.type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver.subtype == PTY_TYPE_MASTER) {
		if (--tty->link->count < 0) {
			printk("release_dev: bad pty slave count (%d) for %s\n",
			       tty->count, tty_name(tty));
			tty->link->count = 0;
		}
	}
	if (--tty->count < 0) {
		printk("release_dev: bad tty->count (%d) for %s\n",
		       tty->count, tty_name(tty));
		tty->count = 0;
	}
	if (tty->count)
		return;

	/*
	 * Sanity check --- if tty->count is zero, there shouldn't be
	 * any waiters on tty->read_wait or tty->write_wait.  But just
	 * in case....
	 */
	while (1) {
		if (waitqueue_active(&tty->read_wait)) {
			printk("release_dev: %s: read_wait active?!?\n",
			       tty_name(tty));
			wake_up(&tty->read_wait);
		} else if (waitqueue_active(&tty->write_wait)) {
			printk("release_dev: %s: write_wait active?!?\n",
			       tty_name(tty));
			wake_up(&tty->write_wait);
		} else
			break;
		schedule();
	}
	
	/*
	 * We're committed; at this point, we must not block!
	 */
	if (o_tty) {
		if (o_tty->count)
			return;
		tty->driver.other->table[idx] = NULL;
		tty->driver.other->termios[idx] = NULL;
		kfree_s(o_tp, sizeof(struct termios));
	}
	
#ifdef TTY_DEBUG_HANGUP
	printk("freeing tty structure...");
#endif
	tty->flags |= (1 << TTY_CLOSING);

	/*
	 * Make sure there aren't any processes that still think this
	 * tty is their controlling tty.
	 */
	read_lock(&tasklist_lock);
	for_each_task(p) {
		if (p->tty == tty)
			p->tty = NULL;
		if (o_tty && p->tty == o_tty)
			p->tty = NULL;
	}
	read_unlock(&tasklist_lock);

	/*
	 * Shutdown the current line discipline, and reset it to
	 * N_TTY.
	 */
	if (tty->ldisc.close)
		(tty->ldisc.close)(tty);
	tty->ldisc = ldiscs[N_TTY];
	tty->termios->c_line = N_TTY;
	if (o_tty) {
		if (o_tty->ldisc.close)
			(o_tty->ldisc.close)(o_tty);
		o_tty->ldisc = ldiscs[N_TTY];
	}
	
	tty->driver.table[idx] = NULL;
	if (tty->driver.flags & TTY_DRIVER_RESET_TERMIOS) {
		tty->driver.termios[idx] = NULL;
		kfree_s(tp, sizeof(struct termios));
	}
	if (tty == redirect || o_tty == redirect)
		redirect = NULL;
	/*
	 * Make sure that the tty's task queue isn't activated.  If it
	 * is, take it out of the linked list.
	 */
	spin_lock_irq(&tqueue_lock);
	if (tty->flip.tqueue.sync) {
		struct tq_struct *tq, *prev;

		for (tq=tq_timer, prev=0; tq; prev=tq, tq=tq->next) {
			if (tq == &tty->flip.tqueue) {
				if (prev)
					prev->next = tq->next;
				else
					tq_timer = tq->next;
				break;
			}
		}
	}
	spin_unlock_irq(&tqueue_lock);
	tty->magic = 0;
	(*tty->driver.refcount)--;
	free_page((unsigned long) tty);
	filp->private_data = 0;
	if (o_tty) {
		o_tty->magic = 0;
		(*o_tty->driver.refcount)--;
		free_page((unsigned long) o_tty);
	}
}

/*
 * tty_open and tty_release keep up the tty count that contains the
 * number of opens done on a tty. We cannot use the inode-count, as
 * different inodes might point to the same tty.
 *
 * Open-counting is needed for pty masters, as well as for keeping
 * track of serial lines: DTR is dropped when the last close happens.
 * (This is not done solely through tty->count, now.  - Ted 1/27/92)
 *
 * The termios state of a pty is reset on first open so that
 * settings don't persist across reuse.
 */
static int tty_open(struct inode * inode, struct file * filp)
{
	struct tty_struct *tty;
	int minor;
	int noctty, retval;
	kdev_t device;
	unsigned short saved_flags;

	saved_flags = filp->f_flags;
retry_open:
	noctty = filp->f_flags & O_NOCTTY;
	device = inode->i_rdev;
	if (device == TTY_DEV) {
		if (!current->tty)
			return -ENXIO;
		device = current->tty->device;
		filp->f_flags |= O_NONBLOCK; /* Don't let /dev/tty block */
		/* noctty = 1; */
	}
	if (device == CONSOLE_DEV) {
		struct console *c = console_drivers;
		while(c && !c->device)
			c = c->next;
		if (!c)
                        return -ENODEV;
                device = c->device();
		noctty = 1;
	}
	minor = MINOR(device);

	retval = init_dev(device, &tty);
	if (retval)
		return retval;
	filp->private_data = tty;
	check_tty_count(tty, "tty_open");
	if (tty->driver.type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver.subtype == PTY_TYPE_MASTER)
		noctty = 1;
#ifdef TTY_DEBUG_HANGUP
	printk("opening %s...", tty_name(tty));
#endif
	if (tty->driver.open)
		retval = tty->driver.open(tty, filp);
	else
		retval = -ENODEV;
	filp->f_flags = saved_flags;

	if (!retval && test_bit(TTY_EXCLUSIVE, &tty->flags) && !suser())
		retval = -EBUSY;

	if (retval) {
#ifdef TTY_DEBUG_HANGUP
		printk("error %d in opening %s...", retval, tty_name(tty));
#endif

		release_dev(filp);
		if (retval != -ERESTARTSYS)
			return retval;
		if (current->signal & ~current->blocked)
			return retval;
		schedule();
		/*
		 * Need to reset f_op in case a hangup happened.
		 */
		filp->f_op = &tty_fops;
		goto retry_open;
	}
	if (!noctty &&
	    current->leader &&
	    !current->tty &&
	    tty->session == 0) {
		current->tty = tty;
		current->tty_old_pgrp = 0;
		tty->session = current->session;
		tty->pgrp = current->pgrp;
	}
	return 0;
}

/*
 * Note that releasing a pty master also releases the child, so
 * we have to make the redirection checks after that and on both
 * sides of a pty.
 */
static int tty_release(struct inode * inode, struct file * filp)
{
	release_dev(filp);
	return 0;
}

static unsigned int tty_poll(struct file * filp, poll_table * wait)
{
	struct tty_struct * tty;

	tty = (struct tty_struct *)filp->private_data;
	if (tty_paranoia_check(tty, filp->f_inode->i_rdev, "tty_poll"))
		return 0;

	if (tty->ldisc.poll)
		return (tty->ldisc.poll)(tty, filp, wait);
	return 0;
}

/*
 * fasync_helper() is used by some character device drivers (mainly mice)
 * to set up the fasync queue. It returns negative on error, 0 if it did
 * no changes and positive if it added/deleted the entry.
 */
int fasync_helper(struct inode * inode, struct file * filp, int on, struct fasync_struct **fapp)
{
	struct fasync_struct *fa, **fp;
	unsigned long flags;

	for (fp = fapp; (fa = *fp) != NULL; fp = &fa->fa_next) {
		if (fa->fa_file == filp)
			break;
	}

	if (on) {
		if (fa)
			return 0;
		fa = (struct fasync_struct *)kmalloc(sizeof(struct fasync_struct), GFP_KERNEL);
		if (!fa)
			return -ENOMEM;
		fa->magic = FASYNC_MAGIC;
		fa->fa_file = filp;
		save_flags(flags);
		cli();
		fa->fa_next = *fapp;
		*fapp = fa;
		restore_flags(flags);
		return 1;
	}
	if (!fa)
		return 0;
	save_flags(flags);
	cli();
	*fp = fa->fa_next;
	restore_flags(flags);
	kfree(fa);
	return 1;
}

static int tty_fasync(struct inode * inode, struct file * filp, int on)
{
	struct tty_struct * tty;
	int retval;

	tty = (struct tty_struct *)filp->private_data;
	if (tty_paranoia_check(tty, inode->i_rdev, "tty_fasync"))
		return 0;
	
	retval = fasync_helper(inode, filp, on, &tty->fasync);
	if (retval <= 0)
		return retval;

	if (on) {
		if (!waitqueue_active(&tty->read_wait))
			tty->minimum_to_wake = 1;
		if (filp->f_owner == 0) {
			if (tty->pgrp)
				filp->f_owner = -tty->pgrp;
			else
				filp->f_owner = current->pid;
		}
	} else {
		if (!tty->fasync && !waitqueue_active(&tty->read_wait))
			tty->minimum_to_wake = N_TTY_BUF_SIZE;
	}
	return 0;
}

static int tiocsti(struct tty_struct *tty, char * arg)
{
	char ch, mbz = 0;

	if ((current->tty != tty) && !suser())
		return -EPERM;
	if (get_user(ch, arg))
		return -EFAULT;
	tty->ldisc.receive_buf(tty, &ch, &mbz, 1);
	return 0;
}

static int tiocgwinsz(struct tty_struct *tty, struct winsize * arg)
{
	if (copy_to_user(arg, &tty->winsize, sizeof(*arg)))
		return -EFAULT;
	return 0;
}

static int tiocswinsz(struct tty_struct *tty, struct tty_struct *real_tty,
	struct winsize * arg)
{
	struct winsize tmp_ws;

	if (copy_from_user(&tmp_ws, arg, sizeof(*arg)))
		return -EFAULT;
	if (!memcmp(&tmp_ws, &tty->winsize, sizeof(*arg)))
		return 0;
	if (tty->pgrp > 0)
		kill_pg(tty->pgrp, SIGWINCH, 1);
	if ((real_tty->pgrp != tty->pgrp) && (real_tty->pgrp > 0))
		kill_pg(real_tty->pgrp, SIGWINCH, 1);
	tty->winsize = tmp_ws;
	real_tty->winsize = tmp_ws;
	return 0;
}

static int tioccons(struct tty_struct *tty, struct tty_struct *real_tty)
{
	if (tty->driver.type == TTY_DRIVER_TYPE_CONSOLE) {
		if (!suser())
			return -EPERM;
		redirect = NULL;
		return 0;
	}
	if (redirect)
		return -EBUSY;
	redirect = real_tty;
	return 0;
}


static int fionbio(struct file *file, int *arg)
{
	int nonblock;

	if (get_user(nonblock, arg))
		return -EFAULT;

	if (nonblock)
		file->f_flags |= O_NONBLOCK;
	else
		file->f_flags &= ~O_NONBLOCK;
	return 0;
}

static int tiocsctty(struct tty_struct *tty, int arg)
{
	if (current->leader &&
	    (current->session == tty->session))
		return 0;
	/*
	 * The process must be a session leader and
	 * not have a controlling tty already.
	 */
	if (!current->leader || current->tty)
		return -EPERM;
	if (tty->session > 0) {
		/*
		 * This tty is already the controlling
		 * tty for another session group!
		 */
		if ((arg == 1) && suser()) {
			/*
			 * Steal it away
			 */
			struct task_struct *p;

			read_lock(&tasklist_lock);
			for_each_task(p)
				if (p->tty == tty)
					p->tty = NULL;
			read_unlock(&tasklist_lock);
		} else
			return -EPERM;
	}
	current->tty = tty;
	current->tty_old_pgrp = 0;
	tty->session = current->session;
	tty->pgrp = current->pgrp;
	return 0;
}

static int tiocgpgrp(struct tty_struct *tty, struct tty_struct *real_tty, pid_t *arg)
{
	/*
	 * (tty == real_tty) is a cheap way of
	 * testing if the tty is NOT a master pty.
	 */
	if (tty == real_tty && current->tty != real_tty)
		return -ENOTTY;
	return put_user(real_tty->pgrp, arg);
}

static int tiocspgrp(struct tty_struct *tty, struct tty_struct *real_tty, pid_t *arg)
{
	pid_t pgrp;
	int retval = tty_check_change(real_tty);

	if (retval == -EIO)
		return -ENOTTY;
	if (retval)
		return retval;
	if (!current->tty ||
	    (current->tty != real_tty) ||
	    (real_tty->session != current->session))
		return -ENOTTY;
	get_user(pgrp, (pid_t *) arg);
	if (pgrp < 0)
		return -EINVAL;
	if (session_of_pgrp(pgrp) != current->session)
		return -EPERM;
	real_tty->pgrp = pgrp;
	return 0;
}

static int tiocttygstruct(struct tty_struct *tty, struct tty_struct *arg)
{
	if (copy_to_user(arg, tty, sizeof(*arg)))
		return -EFAULT;
	return 0;
}

static int tiocsetd(struct tty_struct *tty, int *arg)
{
	int retval, ldisc;

	retval = tty_check_change(tty);
	if (retval)
		return retval;
	retval = get_user(ldisc, arg);
	if (retval)
		return retval;
	return tty_set_ldisc(tty, ldisc);
}

/*
 * Split this up, as gcc can choke on it otherwise..
 */
static int tty_ioctl(struct inode * inode, struct file * file,
		     unsigned int cmd, unsigned long arg)
{
	struct tty_struct *tty, *real_tty;
	
	tty = (struct tty_struct *)file->private_data;
	if (tty_paranoia_check(tty, inode->i_rdev, "tty_ioctl"))
		return -EINVAL;

	real_tty = tty;
	if (tty->driver.type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver.subtype == PTY_TYPE_MASTER)
		real_tty = tty->link;

	switch (cmd) {
		case TIOCSTI:
			return tiocsti(tty, (char *)arg);
		case TIOCGWINSZ:
			return tiocgwinsz(tty, (struct winsize *) arg);
		case TIOCSWINSZ:
			return tiocswinsz(tty, real_tty, (struct winsize *) arg);
		case TIOCCONS:
			return tioccons(tty, real_tty);
		case FIONBIO:
			return fionbio(file, (int *) arg);
		case TIOCEXCL:
			set_bit(TTY_EXCLUSIVE, &tty->flags);
			return 0;
		case TIOCNXCL:
			clear_bit(TTY_EXCLUSIVE, &tty->flags);
			return 0;
		case TIOCNOTTY:
			if (current->tty != tty)
				return -ENOTTY;
			if (current->leader)
				disassociate_ctty(0);
			current->tty = NULL;
			return 0;
		case TIOCSCTTY:
			return tiocsctty(tty, arg);
		case TIOCGPGRP:
			return tiocgpgrp(tty, real_tty, (pid_t *) arg);
		case TIOCSPGRP:
			return tiocspgrp(tty, real_tty, (pid_t *) arg);
		case TIOCGETD:
			return put_user(tty->ldisc.num, (int *) arg);
		case TIOCSETD:
			return tiocsetd(tty, (int *) arg);
#ifdef CONFIG_VT
		case TIOCLINUX:
			return tioclinux(tty, arg);
#endif
		case TIOCTTYGSTRUCT:
			return tiocttygstruct(tty, (struct tty_struct *) arg);
	}
	if (tty->driver.ioctl) {
		int retval = (tty->driver.ioctl)(tty, file, cmd, arg);
		if (retval != -ENOIOCTLCMD)
			return retval;
	}
	if (tty->ldisc.ioctl) {
		int retval = (tty->ldisc.ioctl)(tty, file, cmd, arg);
		if (retval != -ENOIOCTLCMD)
			return retval;
	}
	return -EINVAL;
}


/*
 * This implements the "Secure Attention Key" ---  the idea is to
 * prevent trojan horses by killing all processes associated with this
 * tty when the user hits the "Secure Attention Key".  Required for
 * super-paranoid applications --- see the Orange Book for more details.
 * 
 * This code could be nicer; ideally it should send a HUP, wait a few
 * seconds, then send a INT, and then a KILL signal.  But you then
 * have to coordinate with the init process, since all processes associated
 * with the current tty must be dead before the new getty is allowed
 * to spawn.
 */
void do_SAK( struct tty_struct *tty)
{
#ifdef TTY_SOFT_SAK
	tty_hangup(tty);
#else
	struct task_struct *p;
	int session;
	int		i;
	struct file	*filp;
	
	if (!tty)
		return;
	session  = tty->session;
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	read_lock(&tasklist_lock);
	for_each_task(p) {
		if ((p->tty == tty) ||
		    ((session > 0) && (p->session == session)))
			send_sig(SIGKILL, p, 1);
		else if (p->files) {
			for (i=0; i < NR_OPEN; i++) {
				filp = p->files->fd[i];
				if (filp && (filp->f_op == &tty_fops) &&
				    (filp->private_data == tty)) {
					send_sig(SIGKILL, p, 1);
					break;
				}
			}
		}
	}
	read_unlock(&tasklist_lock);
#endif
}

/*
 * This routine is called out of the software interrupt to flush data
 * from the flip buffer to the line discipline.
 */
static void flush_to_ldisc(void *private_)
{
	struct tty_struct *tty = (struct tty_struct *) private_;
	unsigned char	*cp;
	char		*fp;
	int		count;

	if (tty->flip.buf_num) {
		cp = tty->flip.char_buf + TTY_FLIPBUF_SIZE;
		fp = tty->flip.flag_buf + TTY_FLIPBUF_SIZE;
		tty->flip.buf_num = 0;

		cli();
		tty->flip.char_buf_ptr = tty->flip.char_buf;
		tty->flip.flag_buf_ptr = tty->flip.flag_buf;
	} else {
		cp = tty->flip.char_buf;
		fp = tty->flip.flag_buf;
		tty->flip.buf_num = 1;

		cli();
		tty->flip.char_buf_ptr = tty->flip.char_buf + TTY_FLIPBUF_SIZE;
		tty->flip.flag_buf_ptr = tty->flip.flag_buf + TTY_FLIPBUF_SIZE;
	}
	count = tty->flip.count;
	tty->flip.count = 0;
	sti();
	
#if 0
	if (count > tty->max_flip_cnt)
		tty->max_flip_cnt = count;
#endif
	tty->ldisc.receive_buf(tty, cp, fp, count);
}

/*
 * This subroutine initializes a tty structure.
 */
static void initialize_tty_struct(struct tty_struct *tty)
{
	memset(tty, 0, sizeof(struct tty_struct));
	tty->magic = TTY_MAGIC;
	tty->ldisc = ldiscs[N_TTY];
	tty->pgrp = -1;
	tty->flip.char_buf_ptr = tty->flip.char_buf;
	tty->flip.flag_buf_ptr = tty->flip.flag_buf;
	tty->flip.tqueue.routine = flush_to_ldisc;
	tty->flip.tqueue.data = tty;
}

/*
 * The default put_char routine if the driver did not define one.
 */
void tty_default_put_char(struct tty_struct *tty, unsigned char ch)
{
	tty->driver.write(tty, 0, &ch, 1);
}

/*
 * Called by a tty driver to register itself.
 */
int tty_register_driver(struct tty_driver *driver)
{
	int error;

	if (driver->flags & TTY_DRIVER_INSTALLED)
		return 0;

	error = register_chrdev(driver->major, driver->name, &tty_fops);
	if (error < 0)
		return error;
	else if(driver->major == 0)
		driver->major = error;

	if (!driver->put_char)
		driver->put_char = tty_default_put_char;
	
	driver->prev = 0;
	driver->next = tty_drivers;
	if (tty_drivers) tty_drivers->prev = driver;
	tty_drivers = driver;
	
#ifdef CONFIG_PROC_FS
	proc_tty_register_driver(driver);
#endif	
	return error;
}

/*
 * Called by a tty driver to unregister itself.
 */
int tty_unregister_driver(struct tty_driver *driver)
{
	int	retval;
	struct tty_driver *p;
	int	found = 0;
	const char *othername = NULL;
	
	if (*driver->refcount)
		return -EBUSY;

	for (p = tty_drivers; p; p = p->next) {
		if (p == driver)
			found++;
		else if (p->major == driver->major)
			othername = p->name;
	}
	
	if (!found)
		return -ENOENT;

	if (othername == NULL) {
		retval = unregister_chrdev(driver->major, driver->name);
		if (retval)
			return retval;
	} else
		register_chrdev(driver->major, othername, &tty_fops);

	if (driver->prev)
		driver->prev->next = driver->next;
	else
		tty_drivers = driver->next;
	
	if (driver->next)
		driver->next->prev = driver->prev;

#ifdef CONFIG_PROC_FS
	proc_tty_unregister_driver(driver);
#endif
	return 0;
}


/*
 * Initialize the console device. This is called *early*, so
 * we can't necessarily depend on lots of kernel help here.
 * Just do some early initializations, and do the complex setup
 * later.
 */
long console_init(long kmem_start, long kmem_end)
{
	/* Setup the default TTY line discipline. */
	memset(ldiscs, 0, sizeof(ldiscs));
	(void) tty_register_ldisc(N_TTY, &tty_ldisc_N_TTY);

	/*
	 * Set up the standard termios.  Individual tty drivers may 
	 * deviate from this; this is used as a template.
	 */
	memset(&tty_std_termios, 0, sizeof(struct termios));
	memcpy(tty_std_termios.c_cc, INIT_C_CC, NCCS);
	tty_std_termios.c_iflag = ICRNL | IXON;
	tty_std_termios.c_oflag = OPOST | ONLCR;
	tty_std_termios.c_cflag = B38400 | CS8 | CREAD | HUPCL;
	tty_std_termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
		ECHOCTL | ECHOKE | IEXTEN;

	/*
	 * set up the console device so that later boot sequences can 
	 * inform about problems etc..
	 */
#ifdef CONFIG_SERIAL_CONSOLE
	kmem_start = serial_console_init(kmem_start, kmem_end);
#endif
#ifdef CONFIG_VT
	kmem_start = con_init(kmem_start);
#endif
	return kmem_start;
}

static struct tty_driver dev_tty_driver, dev_console_driver;

/*
 * Ok, now we can initialize the rest of the tty devices and can count
 * on memory allocations, interrupts etc..
 */
__initfunc(int tty_init(void))
{
	if (sizeof(struct tty_struct) > PAGE_SIZE)
		panic("size of tty structure > PAGE_SIZE!");

	/*
	 * dev_tty_driver and dev_console_driver are actually magic
	 * devices which get redirected at open time.  Nevertheless,
	 * we register them so that register_chrdev is called
	 * appropriately.
	 */
	memset(&dev_tty_driver, 0, sizeof(struct tty_driver));
	dev_tty_driver.magic = TTY_DRIVER_MAGIC;
	dev_tty_driver.driver_name = "/dev/tty";
	dev_tty_driver.name = dev_tty_driver.driver_name + 5;
	dev_tty_driver.name_base = 0;
	dev_tty_driver.major = TTYAUX_MAJOR;
	dev_tty_driver.minor_start = 0;
	dev_tty_driver.num = 1;
	dev_tty_driver.type = TTY_DRIVER_TYPE_SYSTEM;
	dev_tty_driver.subtype = SYSTEM_TYPE_TTY;
	
	if (tty_register_driver(&dev_tty_driver))
		panic("Couldn't register /dev/tty driver\n");

	dev_console_driver = dev_tty_driver;
	dev_console_driver.driver_name = "/dev/console";
	dev_console_driver.name = dev_console_driver.driver_name + 5;
	dev_console_driver.major = TTY_MAJOR;
	dev_console_driver.type = TTY_DRIVER_TYPE_SYSTEM;
	dev_console_driver.subtype = SYSTEM_TYPE_CONSOLE;

	if (tty_register_driver(&dev_console_driver))
		panic("Couldn't register /dev/console driver\n");

#ifdef CONFIG_VT
	kbd_init();
#endif
#ifdef CONFIG_ESPSERIAL  /* init ESP before rs, so rs doesn't see the port */
	espserial_init();
#endif
#ifdef CONFIG_SERIAL
	rs_init();
#endif
#ifdef CONFIG_CYCLADES
	cy_init();
#endif
#ifdef CONFIG_STALLION
	stl_init();
#endif
#ifdef CONFIG_ISTALLION
	stli_init();
#endif
#ifdef CONFIG_DIGI
	pcxe_init();
#endif
#ifdef CONFIG_DIGIEPCA
	pc_init();
#endif
#ifdef CONFIG_RISCOM8
	riscom8_init();
#endif
	pty_init();
#ifdef CONFIG_VT
	vcs_init();
#endif
	return 0;
}

