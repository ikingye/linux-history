/*
 *  arch/s390/kernel/s390io.c
 *   S/390 common I/O routines
 *
 *  S390 version
 *    Copyright (C) 1999, 2000 IBM Deutschland Entwicklung GmbH,
 *                             IBM Corporation
 *    Author(s): Ingo Adlung (adlung@de.ibm.com)
 */

#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/smp.h>
#include <linux/tasks.h>
#include <linux/smp_lock.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/smp.h>
#include <asm/pgtable.h>
#include <asm/delay.h>
#include <asm/processor.h>
#include <asm/lowcore.h>
#include <asm/s390io.h>
#include <asm/s390dyn.h>
#include <asm/s390mach.h>

#undef  CONFIG_DEBUG_IO
#define CONFIG_DEBUG_CRW

#define REIPL_DEVID_MAGIC 0x87654321

struct s390_irqaction  init_IRQ_action;
unsigned int           highest_subchannel;
ioinfo_t              *ioinfo_head = NULL;
ioinfo_t              *ioinfo_tail = NULL;
ioinfo_t              *ioinfo[__MAX_SUBCHANNELS] = {
	[0 ... (__MAX_SUBCHANNELS-1)] = INVALID_STORAGE_AREA
};

static spinlock_t sync_isc = SPIN_LOCK_UNLOCKED;
                                          // synchronous irq processing lock
static psw_t      io_sync_wait;           // wait PSW for sync IO, prot. by sync_isc
static int        cons_dev          = -1; // identify console device
static int        init_IRQ_complete = 0;
static schib_t    init_schib;
static irb_t      init_irb;
static __u64      irq_IPL_TOD;

/*
 * Dummy controller type for unused interrupts
 */
int  do_none(unsigned int irq, int cpu, struct pt_regs * regs) { return 0;}
int  enable_none(unsigned int irq) { return(-ENODEV); }
int  disable_none(unsigned int irq) { return(-ENODEV); }

struct hw_interrupt_type no_irq_type = {
	"none",
	do_none,
	enable_none,
	disable_none
};

static void init_IRQ_handler( int irq, void *dev_id, struct pt_regs *regs);
static int  s390_setup_irq(unsigned int irq, struct s390_irqaction * new);
static void s390_process_subchannels( void);
static void s390_device_recognition_all( void);
static void s390_device_recognition_irq( int irq);
static int  s390_validate_subchannel( int irq, int enable);
static int  s390_SenseID( int irq, senseid_t *sid, __u8 lpm);
static int  s390_SetPGID( int irq, __u8 lpm, pgid_t *pgid);
static int  s390_SensePGID( int irq, __u8 lpm, pgid_t *pgid);
static int  s390_process_IRQ( unsigned int irq );
static int  disable_subchannel( unsigned int irq);

int  s390_DevicePathVerification( int irq, __u8 domask );

extern int  do_none(unsigned int irq, int cpu, struct pt_regs * regs);
extern int  enable_none(unsigned int irq);
extern int  disable_none(unsigned int irq);
extern void tod_wait(unsigned long usecs);

asmlinkage void do_IRQ( struct pt_regs regs,
                        unsigned int   irq,
                        __u32          s390_intparm );

//
// fix me ! must be removed with 2.3.x and follow-up releases
//
static void * alloc_bootmem( unsigned long size);
static int    free_bootmem( unsigned long buffer, unsigned long size);
static unsigned long memory_start = 0;

void s390_displayhex(char *str,void *ptr,s32 cnt);

void s390_displayhex(char *str,void *ptr,s32 cnt)
{
	s32	cnt1,cnt2,maxcnt2;
	u32	*currptr=(__u32 *)ptr;

	printk("\n%s\n",str);

	for(cnt1=0;cnt1<cnt;cnt1+=16)
	{
		printk("%08X ",(__u32)currptr);
		maxcnt2=cnt-cnt1;
		if(maxcnt2>16)
			maxcnt2=16;
		for(cnt2=0;cnt2<maxcnt2;cnt2+=4)
			printk("%08X ",*currptr++);
		printk("\n");
	}
}


int s390_request_irq_special( int                      irq,
                              io_handler_func_t        io_handler,
                              not_oper_handler_func_t  not_oper_handler,
                              unsigned long            irqflags,
                              const char              *devname,
                              void                    *dev_id)
{
	int               retval;
	struct s390_irqaction *action;

	if (irq >= __MAX_SUBCHANNELS)
		return -EINVAL;

	if ( !io_handler || !dev_id )
		return -EINVAL;

	/*
	 * during init_IRQ() processing we don't have memory
	 *  management yet, thus need to use a statically
	 *  allocated irqaction control block
	 */
	if ( init_IRQ_complete )
	{
		action = (struct s390_irqaction *)
		            kmalloc( sizeof(struct s390_irqaction),
		                     GFP_KERNEL);
	}
	else
	{
		action = &init_IRQ_action;

	} /* endif */

	if (!action)
	{
		return -ENOMEM;

	} /* endif */

	action->handler = io_handler;
	action->flags   = irqflags;
	action->name    = devname;
	action->dev_id  = dev_id;

	retval = s390_setup_irq( irq, action);

	if ( init_IRQ_complete )
	{
		if ( !retval )
		{
			s390_DevicePathVerification( irq, 0 );
		}
		else
		{
			kfree(action);

		} /* endif */

	} /* endif */

	if ( retval == 0 )
	{
		ioinfo[irq]->ui.flags.newreq = 1;
		ioinfo[irq]->nopfunc         = not_oper_handler;  	
	}

	return retval;
}


int s390_request_irq( unsigned int   irq,
                      void           (*handler)(int, void *, struct pt_regs *),
                      unsigned long  irqflags,
                      const char    *devname,
                      void          *dev_id)
{
	int ret;

	ret = s390_request_irq_special( irq,
                                   (io_handler_func_t)handler,
                                   NULL,
                                   irqflags,
                                   devname,
                                   dev_id);

	if ( ret == 0 )
	{
		ioinfo[irq]->ui.flags.newreq = 0;

	} /* endif */

	return( ret);
}

void s390_free_irq(unsigned int irq, void *dev_id)
{
	unsigned long flags;
	int          ret;

	unsigned int count = 0;

	if ( irq >= __MAX_SUBCHANNELS || ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return;

	} /* endif */

	s390irq_spin_lock_irqsave( irq, flags);

#ifdef  CONFIG_KERNEL_DEBUG
	if ( irq != cons_dev )
	{
		printk("Trying to free IRQ%d\n",irq);

	} /* endif */
#endif

	/*
	 * disable the device and reset all IRQ info if
	 *  the IRQ is actually owned by the handler ...
	 */
	if ( ioinfo[irq]->irq_desc.action )
	{
		if (    (dev_id == ioinfo[irq]->irq_desc.action->dev_id  )
		     || (dev_id == (devstat_t *)REIPL_DEVID_MAGIC) )
		{
			/* start deregister */
			ioinfo[irq]->ui.flags.unready = 1;

			do
			{
				ret = disable_subchannel( irq);

				count++;

				if ( ret == -EBUSY )
				{
					int iret;

					/*
					 * kill it !
					 * ... we first try sync and eventually
					 *  try terminating the current I/O by
					 *  an async request, twice halt, then
					 *  clear.
					 */
					if ( count < 2 )
					{              	
						iret = halt_IO( irq,
						                0xC8C1D3E3,
						                DOIO_WAIT_FOR_INTERRUPT );
   	
						if ( iret == -EBUSY )
						{
							halt_IO( irq, 0xC8C1D3E3, 0);
							s390irq_spin_unlock_irqrestore( irq, flags);
							tod_wait( 200000 ); /* 200 ms */
							s390irq_spin_lock_irqsave( irq, flags);

						} /* endif */
					}
					else
					{
						iret = clear_IO( irq,
						                 0x40C3D3D9,
						                 DOIO_WAIT_FOR_INTERRUPT );
   	
						if ( iret == -EBUSY )
						{
							clear_IO( irq, 0xC8C1D3E3, 0);
							s390irq_spin_unlock_irqrestore( irq, flags);
							tod_wait( 1000000 ); /* 1000 ms */
							s390irq_spin_lock_irqsave( irq, flags);

						} /* endif */

					} /* endif */

					if ( count == 2 )
               {
						/* give it a very last try ... */
						disable_subchannel( irq);

						if ( ioinfo[irq]->ui.flags.busy )
					   {
							printk( KERN_CRIT"free_irq(%04X) "
							       "- device %04X busy, retry "
							       "count exceeded\n",
						   	    irq,
						      	 ioinfo[irq]->devstat.devno);

                  } /* endif */
						
						break; /* sigh, let's give up ... */

					} /* endif */

				} /* endif */

			} while ( ret == -EBUSY );

			if ( init_IRQ_complete )
				kfree( ioinfo[irq]->irq_desc.action );

			ioinfo[irq]->irq_desc.action           = NULL;
			ioinfo[irq]->ui.flags.ready            = 0;
			ioinfo[irq]->irq_desc.handler->enable  = enable_none;
			ioinfo[irq]->irq_desc.handler->disable = disable_none;
			ioinfo[irq]->ui.flags.unready          = 0; /* deregister ended */

			ioinfo[irq]->nopfunc = NULL;

			s390irq_spin_unlock_irqrestore( irq, flags);
		}
		else
		{
			s390irq_spin_unlock_irqrestore( irq, flags);

			printk( "free_irq(%04X) : error, "
			        "dev_id does not match !", irq);

		} /* endif */

	}
	else
	{
		s390irq_spin_unlock_irqrestore( irq, flags);

		printk( "free_irq(%04X) : error, "
		        "no action block ... !\n", irq);

	} /* endif */

}

/*
 * Generic enable/disable code
 */
int disable_irq(unsigned int irq)
{
	unsigned long flags;
	int           ret;

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
		return( -ENODEV);

	s390irq_spin_lock_irqsave(irq, flags);

	/*
	 * At this point we may actually have a pending interrupt being active
	 * on another CPU. So don't touch the IRQ_INPROGRESS bit..
	 */
	ioinfo[irq]->irq_desc.status |= IRQ_DISABLED;
	ret = ioinfo[irq]->irq_desc.handler->disable(irq);
	s390irq_spin_unlock_irqrestore(irq, flags);

	synchronize_irq();

	return( ret);
}

int enable_irq(unsigned int irq)
{
	unsigned long flags;
	int           ret;

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
		return( -ENODEV);

	s390irq_spin_lock_irqsave(irq, flags);

	ioinfo[irq]->irq_desc.status = 0;
	ret = ioinfo[irq]->irq_desc.handler->enable(irq);

	s390irq_spin_unlock_irqrestore(irq, flags);

	return(ret);
}

/*
 * Enable IRQ by modifying the subchannel
 */
static int enable_subchannel( unsigned int irq)
{
	int   ret;
	int   ccode;
	int   retry = 5;

	if ( irq > highest_subchannel || irq < 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
		return( -ENODEV);

	/*
	 * If a previous disable request is pending we reset it. However, this
	 *  status implies that the device may (still) be not-operational.
	 */
	if (  ioinfo[irq]->ui.flags.d_disable )
	{
		ioinfo[irq]->ui.flags.d_disable = 0;
		ret                             = 0;
	}
	else
	{
		ccode = stsch(irq, &(ioinfo[irq]->schib) );

		if ( ccode )
		{
			ret = -ENODEV;
		}
		else
		{
			ioinfo[irq]->schib.pmcw.ena = 1;

			if ( irq == cons_dev )
			{
				ioinfo[irq]->schib.pmcw.isc = 7;
			}
			else	
			{
				ioinfo[irq]->schib.pmcw.isc = 3;

			} /* endif */

			do
			{
				ccode = msch( irq, &(ioinfo[irq]->schib) );

				switch (ccode) {
				case 0:
					ret = 0;
					break;

				case 1:
					/*
					 * very bad, requires interrupt alike
					 *  processing, where "rbh" is a dummy
					 *  parameter for interface compatibility
					 *  only. Bottom-half handling cannot be
					 *  required as this must be an
					 *  unsolicited interrupt (!busy).
					 */

					ioinfo[irq]->ui.flags.s_pend = 1;
					s390_process_IRQ( irq );
					ioinfo[irq]->ui.flags.s_pend = 0;

					ret = -EIO;    /* might be overwritten */
					               /* ... on re-driving    */
					               /* ... the msch() */
					retry--;
					break;

				case 2:
					tod_wait(100);	/* allow for recovery */
					ret = -EBUSY;
					retry--;
					break;

				case 3:
					ioinfo[irq]->ui.flags.oper = 0;
					ret = -ENODEV;
					break;

				default:
					printk( KERN_CRIT"enable_subchannel(%04X) "
					        " : ccode 2 on msch() for device "
					        "%04X received !\n",
					        irq,
					        ioinfo[irq]->devstat.devno);

					ret = -ENODEV; // never reached
				}

			} while ( (ccode == 1) && retry );

		} /* endif */

	}  /* endif */

	return( ret );
}


/*
 * Disable IRQ by modifying the subchannel
 */
static int disable_subchannel( unsigned int irq)
{
	int  cc;          /* condition code */
	int  ret;         /* function return value */
	int  retry = 5;

	if ( irq > highest_subchannel )
	{
		ret = -ENODEV;
	}
	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
	}
	else if ( ioinfo[irq]->ui.flags.busy )
	{
		/*
		 * the disable function must not be called while there are
		 *  requests pending for completion !
		 */
		ret = -EBUSY;
	}
	else
	{
		/*
		 * If device isn't operational we have to perform delayed
		 *  disabling when the next interrupt occurs - unless the
		 *  irq is re-requested prior to the interrupt to occur.
		 */
		cc = stsch(irq, &(ioinfo[irq]->schib) );

		if ( cc == 3 )
		{
			ioinfo[irq]->ui.flags.oper      = 0;
			ioinfo[irq]->ui.flags.d_disable = 1;

			ret = 0;
		}
		else // cc == 0
		{
			ioinfo[irq]->schib.pmcw.ena = 0;

			do
			{
				cc = msch( irq, &(ioinfo[irq]->schib) );

				switch (cc) {
				case 0 :
					ret = 0;   /* done */
					break;

				case 1 :
					/*
					 * very bad, requires interrupt alike
  					 *  processing, where "rbh" is a dummy
					 *  parm for interface compatibility
					 *  only. Bottom-half handling cannot
					 *  be required as this must be an
					 *  unsolicited interrupt (!busy).
					 */
					ioinfo[irq]->ui.flags.s_pend = 1;
					s390_process_IRQ( irq );
					ioinfo[irq]->ui.flags.s_pend = 0;

					ret = -EIO; /* might be overwritten  */
					            /* ... on re-driving the */
					            /* ... msch() call       */
					retry--;
					break;

				case 2 :
					/*
					 * *** must not occur !              ***
					 * ***                               ***
					 * *** indicates our internal        ***
					 * *** interrupt accounting is out   ***
					 * ***  of sync ===> panic()         ***
					 */
					printk( KERN_CRIT"disable_subchannel(%04X) "
					        "- unexpected busy condition for "
							  "device %04X received !\n",
					        irq,
					        ioinfo[irq]->devstat.devno);
					ret = -EBUSY;
					break;

				case 3 :
					/*
					 * should hardly occur ?!
					 */
					ioinfo[irq]->ui.flags.oper      = 0;
					ioinfo[irq]->ui.flags.d_disable = 1;

					ret = 0; /* if the device has gone we */
					         /* ... don't need to disable */
					         /* ... it anymore !    */
					break;

				default :
					ret = -ENODEV;  // never reached ...
					break;

				} /* endswitch */

			} while ( (cc == 1) && retry );

		} /* endif */

	} /* endif */

	return( ret);
}


int s390_setup_irq( unsigned int irq, struct s390_irqaction * new)
{
	unsigned long      flags;
	int                rc = 0;

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	/*
	 * The following block of code has to be executed atomically
	 */
	s390irq_spin_lock_irqsave( irq, flags);

	if ( ioinfo[irq]->irq_desc.action == NULL )
	{
		ioinfo[irq]->irq_desc.action           = new;
		ioinfo[irq]->irq_desc.status           = 0;
		ioinfo[irq]->irq_desc.handler->enable  = enable_subchannel;
		ioinfo[irq]->irq_desc.handler->disable = disable_subchannel;
		ioinfo[irq]->irq_desc.handler->handle  = handle_IRQ_event;

		ioinfo[irq]->ui.flags.ready            = 1;

		ioinfo[irq]->irq_desc.handler->enable(irq);
	}
	else
	{
		/*
		 *  interrupt already owned, and shared interrupts
		 *   aren't supported on S/390.
		 */
		rc = -EBUSY;

	} /* endif */

	s390irq_spin_unlock_irqrestore(irq,flags);

	return( rc);
}

/*
 * we need boot memory during init processing but don't have
 *  kmalloc() available yet. Linux 2.3 introduced alloc_bootmem()
 *  we kind of emulate for the time being.
 */
static void * alloc_bootmem( unsigned long size)
{
   void * ret = (void *)memory_start;

   memory_start += size;

   return( ret );
}

int free_bootmem( unsigned long buffer, unsigned long size)
{
   int ret = 0;

	/*
	 *	We don't have buffer management, thus a free
	 *  must follow the matching alloc.
	 */	
	if ( buffer == (memory_start - size) )
   	memory_start -= size;
	else
		ret = -EINVAL;

   return( ret );
}

unsigned long s390_init_IRQ( unsigned long memstart)
{
	unsigned long flags;     /* PSW flags */
	long          cr6 __attribute__ ((aligned (8)));

        memory_start = memstart;

	// Hopefully bh_count's will get set when we copy the prefix lowcore
	// structure to other CPI's ( DJB )
	atomic_set(&S390_lowcore.local_bh_count,0);
	atomic_set(&S390_lowcore.local_irq_count,0);

	asm volatile ("STCK %0" : "=m" (irq_IPL_TOD));

	/*
	 * As we don't know about the calling environment
	 *  we assure running disabled. Before leaving the
	 *  function we resestablish the old environment.
	 *
	 * Note : as we don't need a system wide lock, therefore
	 *        we shouldn't use cli(), but __cli() as this
	 *        affects the current CPU only.
	 */
	__save_flags(flags);
	__cli();

	/*
	 * disable all interrupts
	 */
	cr6 = 0;
	asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

	s390_process_subchannels();

	/*
	 * enable default I/O-interrupt sublass 3
	 */
	cr6 = 0x10000000;
	asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

	s390_device_recognition_all();

	init_IRQ_complete = 1;

	__restore_flags(flags);

        return( memory_start );
}


/*
 * dummy handler, used during init_IRQ() processing for compatibility only
 */
void  init_IRQ_handler( int irq, void *dev_id, struct pt_regs *regs)
{
   /* this is a dummy handler only ... */
}


int s390_start_IO( int            irq,      /* IRQ */
                   ccw1_t        *cpa,      /* logical channel prog addr */
                   unsigned long  user_intparm,  /* interruption parameter */
                   __u8           lpm,      /* logical path mask */
                   unsigned long  flag)     /* flags */
{
	int            ccode;
	unsigned long  psw_flags;

	int            sync_isc_locked = 0;
	int            ret             = 0;

	/*
	 * The flag usage is mutal exclusive ...
	 */
	if (    (flag & DOIO_EARLY_NOTIFICATION)
	     && (flag & DOIO_REPORT_ALL     ) )
	{
		return( -EINVAL );

	} /* endif */

	/*
	 * setup ORB
	 */
  	ioinfo[irq]->orb.intparm = (__u32)&ioinfo[irq]->u_intparm;
	ioinfo[irq]->orb.fmt     = 1;

	ioinfo[irq]->orb.pfch = !(flag & DOIO_DENY_PREFETCH);
	ioinfo[irq]->orb.spnd =  (flag & DOIO_ALLOW_SUSPEND ? TRUE : FALSE);
	ioinfo[irq]->orb.ssic =  (    (flag & DOIO_ALLOW_SUSPEND )
	                           && (flag & DOIO_SUPPRESS_INTER) );

	if ( flag & DOIO_VALID_LPM )
	{
		ioinfo[irq]->orb.lpm = lpm;
	}
	else
	{
		ioinfo[irq]->orb.lpm = ioinfo[irq]->opm;

	} /* endif */

	ioinfo[irq]->orb.cpa = (__u32)virt_to_phys( cpa);

	/*
	 * If sync processing was requested we lock the sync ISC, modify the
	 *  device to present interrupts for this ISC only and switch the
	 *  CPU to handle this ISC + the console ISC exclusively.
	 */
	if ( flag & DOIO_WAIT_FOR_INTERRUPT )
	{
		//
		// check whether we run recursively (sense processing)
		//
		if ( !ioinfo[irq]->ui.flags.syncio )
		{
			spin_lock_irqsave( &sync_isc, psw_flags);
 	
			ret = enable_cpu_sync_isc( irq);

			if ( ret )
			{
				spin_unlock_irqrestore( &sync_isc, psw_flags);
				return( ret);
			}
			else
			{
				sync_isc_locked              = 1; // local
				ioinfo[irq]->ui.flags.syncio = 1; // global

			} /* endif */  	
 	
		} /* endif */

	} /* endif */

	if ( flag & DOIO_DONT_CALL_INTHDLR )
	{
		ioinfo[irq]->ui.flags.repnone = 1;

   } /* endif */

	/*
	 * Issue "Start subchannel" and process condition code
	 */
	ccode = ssch( irq, &(ioinfo[irq]->orb) );

	switch ( ccode ) {
	case 0:

		if ( !ioinfo[irq]->ui.flags.w4sense )
		{
			/*
			 * init the device driver specific devstat irb area
			 *
			 * Note : don�t clear saved irb info in case of sense !
			 */
			memset( &((devstat_t *)ioinfo[irq]->irq_desc.action->dev_id)->ii.irb,
				'\0', sizeof( irb_t) );
		} /* endif */

		memset( &ioinfo[irq]->devstat.ii.irb,
		        '\0',
		        sizeof( irb_t) );

		/*
		 * initialize device status information
		 */
		ioinfo[irq]->ui.flags.busy   = 1;
		ioinfo[irq]->ui.flags.doio   = 1;

		ioinfo[irq]->u_intparm       = user_intparm;
		ioinfo[irq]->devstat.cstat   = 0;
		ioinfo[irq]->devstat.dstat   = 0;
		ioinfo[irq]->devstat.lpum    = 0;
		ioinfo[irq]->devstat.flag    = DEVSTAT_START_FUNCTION;
		ioinfo[irq]->devstat.scnt    = 0;

		ioinfo[irq]->ui.flags.fast   = 0;
		ioinfo[irq]->ui.flags.repall = 0;

		/*
		 * Check for either early (FAST) notification requests
		 *  or if we are to return all interrupt info.
		 * Default is to call IRQ handler at secondary status only
		 */
		if ( flag & DOIO_EARLY_NOTIFICATION )
		{
			ioinfo[irq]->ui.flags.fast = 1;
		}
		else if ( flag & DOIO_REPORT_ALL )
		{
			ioinfo[irq]->ui.flags.repall = 1;

		} /* endif */

		ioinfo[irq]->ulpm = ioinfo[irq]->orb.lpm;
		
		/*
		 * If synchronous I/O processing is requested, we have
		 *  to wait for the corresponding interrupt to occur by
		 *  polling the interrupt condition. However, as multiple
		 *  interrupts may be outstanding, we must not just wait
		 *  for the first interrupt, but must poll until ours
		 *  pops up.
		 */
		if ( flag & DOIO_WAIT_FOR_INTERRUPT )
		{
			psw_t            io_new_psw;
			int              ccode;
			uint64_t         time_start;    	
			uint64_t         time_curr;    	

			int              ready    = 0;
			int              io_sub   = -1;
			struct _lowcore *lc       = NULL;
			int              do_retry = 1;

			/*
			 * We shouldn't perform a TPI loop, waiting for an
			 *  interrupt to occur, but should load a WAIT PSW
			 *  instead. Otherwise we may keep the channel subsystem
			 *  busy, not able to present the interrupt. When our
			 *  sync. interrupt arrived we reset the I/O old PSW to
			 *  its original value.
			 */
			memcpy( &io_new_psw, &lc->io_new_psw, sizeof(psw_t));

			ccode = iac();

			switch (ccode) {
			case 0:  		// primary-space
				io_sync_wait.mask =   _IO_PSW_MASK
				                    | _PSW_PRIM_SPACE_MODE
				                    | _PSW_IO_WAIT;
				break;
			case 1:			// secondary-space
				io_sync_wait.mask =   _IO_PSW_MASK
				                    | _PSW_SEC_SPACE_MODE
				                    | _PSW_IO_WAIT;
				break;
			case 2:			// access-register
				io_sync_wait.mask =   _IO_PSW_MASK
				                    | _PSW_ACC_REG_MODE
				                    | _PSW_IO_WAIT;
				break;
			case 3:			// home-space	
				io_sync_wait.mask =   _IO_PSW_MASK
				                    | _PSW_HOME_SPACE_MODE
				                    | _PSW_IO_WAIT;
				break;
			default:
				panic( "start_IO() : unexpected "
				       "address-space-control %d\n",
				       ccode);
				break;
			} /* endswitch */

			io_sync_wait.addr = FIX_PSW(&&io_wakeup);

			/*
			 * Martin didn't like modifying the new PSW, now we take
			 *  a fast exit in do_IRQ() instead
			 */
			*(__u32 *)__LC_SYNC_IO_WORD  = 1;

			asm volatile ("STCK %0" : "=m" (time_start));

			time_start = time_start >> 32;

			do
			{
				if ( flag & DOIO_TIMEOUT )
				{
					tpi_info_t tpi_info;

					do
					{
						if ( tpi(&tpi_info) == 1 )
						{
							io_sub = tpi_info.irq;
							break;
						}
						else
						{
							tod_wait(100); /* usecs */
							asm volatile ("STCK %0" : "=m" (time_curr));

							if ( ((time_curr >> 32) - time_start ) >= 3 )
								do_retry = 0;          							

						} /* endif */
				
					} while ( do_retry );
				}
				else
				{
					asm volatile ("lpsw %0" : : "m" (io_sync_wait));

io_wakeup:
					io_sub  = (__u32)*(__u16 *)__LC_SUBCHANNEL_NR;

				} /* endif */

 				if ( do_retry )
					ready = s390_process_IRQ( io_sub );

				/*
				 * surrender when retry count's exceeded ...
				 */
			} while ( !(     ( io_sub == irq )
			              && ( ready  == 1   ))
			            && do_retry             );

			*(__u32 *)__LC_SYNC_IO_WORD = 0;

			if ( !do_retry )
				ret = -ETIMEDOUT;

		} /* endif */

		break;

	case 1 :            /* status pending */

		ioinfo[irq]->devstat.flag =   DEVSTAT_START_FUNCTION
		                            | DEVSTAT_STATUS_PENDING;

		/*
		 * initialize the device driver specific devstat irb area
		 */
		memset( &((devstat_t *) ioinfo[irq]->irq_desc.action->dev_id)->ii.irb,
		        '\0', sizeof( irb_t) );

		/*
		 * Let the common interrupt handler process the pending status.
		 *  However, we must avoid calling the user action handler, as
		 *  it won't be prepared to handle a pending status during
		 *  do_IO() processing inline. This also implies that process_IRQ
		 *  must terminate synchronously - especially if device sensing
		 *  is required.
		 */
		ioinfo[irq]->ui.flags.s_pend   = 1;
		ioinfo[irq]->ui.flags.busy     = 1;
		ioinfo[irq]->ui.flags.doio     = 1;

		s390_process_IRQ( irq );

		ioinfo[irq]->ui.flags.s_pend   = 0;
		ioinfo[irq]->ui.flags.busy     = 0;
		ioinfo[irq]->ui.flags.doio     = 0;

		ioinfo[irq]->ui.flags.repall   = 0;
		ioinfo[irq]->ui.flags.w4final  = 0;

		ioinfo[irq]->devstat.flag     |= DEVSTAT_FINAL_STATUS;

		/*
		 * In multipath mode a condition code 3 implies the last path
		 *  has gone, except we have previously restricted the I/O to
		 *  a particular path. A condition code 1 (0 won't occur)
		 *  results in return code EIO as well as 3 with another path
		 *  than the one used (i.e. path available mask is non-zero).
		 */
		if ( ioinfo[irq]->devstat.ii.irb.scsw.cc == 3 )
		{
			if ( flag & DOIO_VALID_LPM )
			{
				ioinfo[irq]->opm &= ~(ioinfo[irq]->devstat.ii.irb.esw.esw1.lpum);
			}
			else
			{
				ioinfo[irq]->opm = 0;

			} /* endif */
	
			if ( ioinfo[irq]->opm == 0 ) 	
			{
				ret                         = -ENODEV;
				ioinfo[irq]->ui.flags.oper  = 0;
         }
			else
			{
				ret = -EIO;

         } /* endif */

			ioinfo[irq]->devstat.flag  |= DEVSTAT_NOT_OPER;

#ifdef CONFIG_DEBUG_IO
			{
			char buffer[80];

			stsch(irq, &(ioinfo[irq]->schib) );

			sprintf( buffer, "s390_start_IO(%04X) - irb for "
			         "device %04X, after status pending\n",
			         irq,
			         ioinfo[irq]->devstat.devno );

			s390_displayhex( buffer,
			                 &(ioinfo[irq]->devstat.ii.irb) ,
			                 sizeof(irb_t));

			sprintf( buffer, "s390_start_IO(%04X) - schib for "
			         "device %04X, after status pending\n",
			         irq,
			         ioinfo[irq]->devstat.devno );

			s390_displayhex( buffer,
			                 &(ioinfo[irq]->schib) ,
			                 sizeof(schib_t));


			if (ioinfo[irq]->devstat.flag & DEVSTAT_FLAG_SENSE_AVAIL)
			{
				sprintf( buffer, "s390_start_IO(%04X) - sense "
				         "data for "
				         "device %04X, after status pending\n",
				         irq,
				         ioinfo[irq]->devstat.devno );

				s390_displayhex( buffer,
		((devstat_t *)(ioinfo[irq]->irq_desc.action->dev_id))->ii.sense.data,
		((devstat_t *)(ioinfo[irq]->irq_desc.action->dev_id))->rescnt);

			} /* endif */
			}
#endif
		}
		else
		{
			ret                         = -EIO;
			ioinfo[irq]->devstat.flag  &= ~DEVSTAT_NOT_OPER;
			ioinfo[irq]->ui.flags.oper  = 1;

		} /* endif */

		break;

	case 2 :            /* busy */

		ret = -EBUSY;
		break;

	default:            /* device/path not operational */
		
		if ( flag & DOIO_VALID_LPM )
		{
			ioinfo[irq]->opm &= ~lpm;
		}
		else
		{
			ioinfo[irq]->opm = 0;

		} /* endif */
	
		if ( ioinfo[irq]->opm == 0 ) 	
		{
			ioinfo[irq]->ui.flags.oper  = 0;
			ioinfo[irq]->devstat.flag  |= DEVSTAT_NOT_OPER;

		} /* endif */

		ret = -ENODEV;

		memcpy( ioinfo[irq]->irq_desc.action->dev_id,
		        &(ioinfo[irq]->devstat),
		        sizeof( devstat_t) );

#ifdef CONFIG_DEBUG_IO
		{
			char buffer[80];

   			stsch(irq, &(ioinfo[irq]->schib) );

			sprintf( buffer, "s390_start_IO(%04X) - schib for "
			         "device %04X, after 'not oper' status\n",
			         irq,
			         ioinfo[irq]->devstat.devno );

			s390_displayhex( buffer,
			                 &(ioinfo[irq]->schib),
			                 sizeof(schib_t));
      	}
#endif
		break;

	} /* endswitch */

	if ( sync_isc_locked )
	{
		int iret;
		int retry = 5;
		int halt  = 0;	

		do
		{
			iret = disable_cpu_sync_isc( irq );
			retry--;

			/* try stopping it ... */
			if ( (iret == -EBUSY) && !halt )
			{
				halt_IO( irq, 0x00004711, 0 );
				halt = 1;

			} /* endif */

			tod_wait( 100);

		} while ( retry && (iret == -EBUSY ) );

		sync_isc_locked              = 0;    // local setting
		ioinfo[irq]->ui.flags.syncio = 0;    // global setting

		spin_unlock_irqrestore( &sync_isc, psw_flags);

	} /* endif */

	if ( flag & DOIO_DONT_CALL_INTHDLR )
	{
		ioinfo[irq]->ui.flags.repnone = 0;

   } /* endif */

	return( ret);
}

int do_IO( int            irq,          /* IRQ */
           ccw1_t        *cpa,          /* channel program address */
           unsigned long  user_intparm, /* interruption parameter */
           __u8           lpm,          /* logical path mask */
           unsigned long  flag)         /* flags : see above */
{
	int ret = 0;

	if ( irq > highest_subchannel || irq < 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	/* handler registered ? or free_irq() in process already ? */
	if ( !ioinfo[irq]->ui.flags.ready || ioinfo[irq]->ui.flags.unready )
	{
		return( -ENODEV );

	} /* endif */

	/*
	 * Note: We ignore the device operational status - if not operational,
	 *        the SSCH will lead to an -ENODEV condition ...
	 */
	if ( !ioinfo[irq]->ui.flags.busy )         /* last I/O completed ? */
	{
		ret = s390_start_IO( irq, cpa, user_intparm, lpm, flag);
	}
	else if ( ioinfo[irq]->ui.flags.fast )
	{
		/*
		 * If primary status was received and ending status is missing,
		 *  the device driver won't be notified on the ending status
		 *  if early (fast) interrupt notification was requested.
		 *  Therefore we have to queue the next incoming request. If
		 *  halt_IO() is issued while there is a request queued, a HSCH
		 *  needs to be issued and the queued request must be deleted
		 *  but its intparm must be returned (see halt_IO() processing)
		 */
		if (     ioinfo[irq]->ui.flags.w4final
           && !ioinfo[irq]->ui.flags.doio_q )
		{
			ioinfo[irq]->qflag    = flag;
			ioinfo[irq]->qcpa     = cpa;
			ioinfo[irq]->qintparm = user_intparm;
			ioinfo[irq]->qlpm     = lpm;
		}
		else
		{
			ret = -EBUSY;

		} /* endif */
	}
	else
	{
		ret = -EBUSY;

	} /* endif */

	return( ret );

}

/*
 * resume suspended I/O operation
 */
int resume_IO( int irq)
{
	int ret = 0;

	if ( irq > highest_subchannel || irq < 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	/*
	 * We allow for 'resume' requests only for active I/O operations
	 */
	if ( ioinfo[irq]->ui.flags.busy )
	{
		int ccode;

		ccode = rsch( irq);

		switch (ccode) {
		case 0 :
			break;

		case 1 :
		  	s390_process_IRQ( irq );
			ret = -EBUSY;
			break;

		case 2 :
			ret = -EINVAL;
			break;

		case 3 :
			/*
			 * useless to wait for request completion
			 *  as device is no longer operational !
			 */
			ioinfo[irq]->ui.flags.oper = 0;
			ioinfo[irq]->ui.flags.busy = 0;
			ret                        = -ENODEV;
			break;

		} /* endswitch */          	
		
	}
	else
	{
		ret = -ENOTCONN;

	} /* endif  */

	return( ret);
}

/*
 * Note: The "intparm" parameter is not used by the halt_IO() function
 *       itself, as no ORB is built for the HSCH instruction. However,
 *       it allows the device interrupt handler to associate the upcoming
 *       interrupt with the halt_IO() request.
 */
int halt_IO( int           irq,
             unsigned long user_intparm,
             unsigned long flag)  /* possible DOIO_WAIT_FOR_INTERRUPT */
{
	int            ret;
	int            ccode;
	unsigned long  psw_flags;

	int            sync_isc_locked = 0;

	if ( irq > highest_subchannel || irq < 0 )
	{
		ret = -ENODEV;
	}

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	/*
	 * we only allow for halt_IO if the device has an I/O handler associated
	 */
	else if ( !ioinfo[irq]->ui.flags.ready )
	{
		ret = -ENODEV;
	}
	/*
	 * we ignore the halt_io() request if ending_status was received but
	 *  a SENSE operation is waiting for completion.
	 */
	else if ( ioinfo[irq]->ui.flags.w4sense )
	{
		ret = 0;
	}
	/*
	 * We don't allow for halt_io with a sync do_IO() requests pending.
	 */
	else if (    ioinfo[irq]->ui.flags.syncio
	          && (flag & DOIO_WAIT_FOR_INTERRUPT))
	{
		ret = -EBUSY;
	}
	else
	{
		/*
		 * If sync processing was requested we lock the sync ISC,
		 *  modify the device to present interrupts for this ISC only
		 *  and switch the CPU to handle this ISC + the console ISC
		 *  exclusively.
		 */
		if ( flag & DOIO_WAIT_FOR_INTERRUPT )
		{
			//
			// check whether we run recursively (sense processing)
			//
			if ( !ioinfo[irq]->ui.flags.syncio )
			{
				spin_lock_irqsave( &sync_isc, psw_flags);
  	
				ret = enable_cpu_sync_isc( irq);

				if ( ret )
				{
					spin_unlock_irqrestore( &sync_isc,
					                        psw_flags);
					return( ret);
				}
				else
				{
					sync_isc_locked              = 1; // local
					ioinfo[irq]->ui.flags.syncio = 1; // global

				} /* endif */  	
  	
			} /* endif */

		} /* endif */

		/*
		 * Issue "Halt subchannel" and process condition code
		 */
		ccode = hsch( irq );

		switch ( ccode ) {
		case 0:

			ioinfo[irq]->ui.flags.haltio = 1;

			if ( !ioinfo[irq]->ui.flags.doio )
			{
				ioinfo[irq]->ui.flags.busy   = 1;
				ioinfo[irq]->u_intparm       = user_intparm;
				ioinfo[irq]->devstat.cstat   = 0;
				ioinfo[irq]->devstat.dstat   = 0;
				ioinfo[irq]->devstat.lpum    = 0;
				ioinfo[irq]->devstat.flag    = DEVSTAT_HALT_FUNCTION;
				ioinfo[irq]->devstat.scnt    = 0;

			}
			else
			{
				ioinfo[irq]->devstat.flag   |= DEVSTAT_HALT_FUNCTION;

			} /* endif */

			/*
			 * If synchronous I/O processing is requested, we have
			 *  to wait for the corresponding interrupt to occur by
			 *  polling the interrupt condition. However, as multiple
			 *  interrupts may be outstanding, we must not just wait
			 *  for the first interrupt, but must poll until ours
			 *  pops up.
			 */
			if ( flag & DOIO_WAIT_FOR_INTERRUPT )
			{
				int              io_sub;
				__u32            io_parm;
				psw_t            io_new_psw;
				int              ccode;
  	
				int              ready = 0;
				struct _lowcore *lc    = NULL;

				/*
				 * We shouldn't perform a TPI loop, waiting for
				 *  an interrupt to occur, but should load a
				 *  WAIT PSW instead. Otherwise we may keep the
				 *  channel subsystem busy, not able to present
				 *  the interrupt. When our sync. interrupt
				 *  arrived we reset the I/O old PSW to its
				 *  original value.
				 */
				memcpy( &io_new_psw,
				        &lc->io_new_psw,
				        sizeof(psw_t));

				ccode = iac();

				switch (ccode) {
				case 0:  		// primary-space
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_PRIM_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 1:			// secondary-space
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_SEC_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 2:			// access-register
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_ACC_REG_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 3:			// home-space	
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_HOME_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				default:
					panic( "halt_IO() : unexpected "
					       "address-space-control %d\n",
					       ccode);
					break;
				} /* endswitch */

				io_sync_wait.addr = FIX_PSW(&&hio_wakeup);

				/*
				 * Martin didn't like modifying the new PSW, now we take
				 *  a fast exit in do_IRQ() instead
				 */
				*(__u32 *)__LC_SYNC_IO_WORD  = 1;

				do
				{

	      		asm volatile ( "lpsw %0" : : "m" (io_sync_wait) );
hio_wakeup:
				   io_parm = *(__u32 *)__LC_IO_INT_PARM;
				   io_sub  = (__u32)*(__u16 *)__LC_SUBCHANNEL_NR;

				   ready = s390_process_IRQ( io_sub );

				} while ( !((io_sub == irq) && (ready == 1)) );

				*(__u32 *)__LC_SYNC_IO_WORD = 0;

			} /* endif */

			ret = 0;
			break;

		case 1 :            /* status pending */
	
			ioinfo[irq]->devstat.flag |= DEVSTAT_STATUS_PENDING;

			/*
			 * initialize the device driver specific devstat irb area
			 */
			memset( &((devstat_t *) ioinfo[irq]->irq_desc.action->dev_id)->ii.irb,
			        '\0', sizeof( irb_t) );

			/*
			 * Let the common interrupt handler process the pending
			 *  status. However, we must avoid calling the user
			 *  action handler, as it won't be prepared to handle
                        *  a pending status during do_IO() processing inline.
			 *  This also implies that s390_process_IRQ must
			 *  terminate synchronously - especially if device
			 *  sensing is required.
			 */
			ioinfo[irq]->ui.flags.s_pend   = 1;
			ioinfo[irq]->ui.flags.busy     = 1;
			ioinfo[irq]->ui.flags.doio     = 1;

			s390_process_IRQ( irq );
			
			ioinfo[irq]->ui.flags.s_pend   = 0;
			ioinfo[irq]->ui.flags.busy     = 0;
			ioinfo[irq]->ui.flags.doio     = 0;
			ioinfo[irq]->ui.flags.repall   = 0;
			ioinfo[irq]->ui.flags.w4final  = 0;

			ioinfo[irq]->devstat.flag     |= DEVSTAT_FINAL_STATUS;

			/*
			 * In multipath mode a condition code 3 implies the last
			 *  path has gone, except we have previously restricted
			 *  the I/O to a particular path. A condition code 1
			 *  (0 won't occur) results in return code EIO as well
			 *  as 3 with another path than the one used (i.e. path available mask is non-zero).
			 */
			if ( ioinfo[irq]->devstat.ii.irb.scsw.cc == 3 )
			{
				ret                         = -ENODEV;
				ioinfo[irq]->devstat.flag  |= DEVSTAT_NOT_OPER;
				ioinfo[irq]->ui.flags.oper  = 0;
			}
			else
			{
				ret                         = -EIO;
				ioinfo[irq]->devstat.flag  &= ~DEVSTAT_NOT_OPER;
				ioinfo[irq]->ui.flags.oper  = 1;

			} /* endif */

			break;

		case 2 :            /* busy */

			ret = -EBUSY;
			break;

		default:            /* device not operational */

			ret = -ENODEV;
			break;

		} /* endswitch */

		if ( sync_isc_locked )
		{
			disable_cpu_sync_isc( irq );
  	
			sync_isc_locked              = 0;    // local setting
			ioinfo[irq]->ui.flags.syncio = 0;    // global setting
  	
			spin_unlock_irqrestore( &sync_isc, psw_flags);
  	
		} /* endif */

	} /* endif */

	return( ret );
}

/*
 * Note: The "intparm" parameter is not used by the clear_IO() function
 *       itself, as no ORB is built for the CSCH instruction. However,
 *       it allows the device interrupt handler to associate the upcoming
 *       interrupt with the clear_IO() request.
 */
int clear_IO( int           irq,
              unsigned long user_intparm,
              unsigned long flag)  /* possible DOIO_WAIT_FOR_INTERRUPT */
{
	int            ret;
	int            ccode;
	unsigned long  psw_flags;

	int            sync_isc_locked = 0;

	if ( irq > highest_subchannel || irq < 0 )
	{
		ret = -ENODEV;
	}

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	/*
	 * we only allow for halt_IO if the device has an I/O handler associated
	 */
	else if ( !ioinfo[irq]->ui.flags.ready )
	{
		ret = -ENODEV;
	}
	/*
	 * we ignore the halt_io() request if ending_status was received but
	 *  a SENSE operation is waiting for completion.
	 */
	else if ( ioinfo[irq]->ui.flags.w4sense )
	{
		ret = 0;
	}
	/*
	 * We don't allow for halt_io with a sync do_IO() requests pending.
	 *  Concurrent I/O is possible in SMP environments only, but the
	 *  sync. I/O request can be gated to one CPU at a time only.
	 */
	else if ( ioinfo[irq]->ui.flags.syncio )
	{
		ret = -EBUSY;
	}
	else
	{
		/*
		 * If sync processing was requested we lock the sync ISC,
		 *  modify the device to present interrupts for this ISC only
		 *  and switch the CPU to handle this ISC + the console ISC
		 *  exclusively.
		 */
		if ( flag & DOIO_WAIT_FOR_INTERRUPT )
		{
			//
			// check whether we run recursively (sense processing)
			//
			if ( !ioinfo[irq]->ui.flags.syncio )
			{
				spin_lock_irqsave( &sync_isc, psw_flags);
  	
				ret = enable_cpu_sync_isc( irq);

				if ( ret )
				{
					spin_unlock_irqrestore( &sync_isc,
					                        psw_flags);
					return( ret);
				}
				else
				{
					sync_isc_locked              = 1; // local
					ioinfo[irq]->ui.flags.syncio = 1; // global

				} /* endif */  	
  	
			} /* endif */

		} /* endif */

		/*
		 * Issue "Halt subchannel" and process condition code
		 */
		ccode = csch( irq );

		switch ( ccode ) {
		case 0:

			ioinfo[irq]->ui.flags.haltio = 1;

			if ( !ioinfo[irq]->ui.flags.doio )
			{
				ioinfo[irq]->ui.flags.busy   = 1;
				ioinfo[irq]->u_intparm       = user_intparm;
				ioinfo[irq]->devstat.cstat   = 0;
				ioinfo[irq]->devstat.dstat   = 0;
				ioinfo[irq]->devstat.lpum    = 0;
				ioinfo[irq]->devstat.flag    = DEVSTAT_CLEAR_FUNCTION;
				ioinfo[irq]->devstat.scnt    = 0;

			}
			else
			{
				ioinfo[irq]->devstat.flag   |= DEVSTAT_CLEAR_FUNCTION;

			} /* endif */

			/*
			 * If synchronous I/O processing is requested, we have
			 *  to wait for the corresponding interrupt to occur by
			 *  polling the interrupt condition. However, as multiple
			 *  interrupts may be outstanding, we must not just wait
			 *  for the first interrupt, but must poll until ours
			 *  pops up.
			 */
			if ( flag & DOIO_WAIT_FOR_INTERRUPT )
			{
				int              io_sub;
				__u32            io_parm;
				psw_t            io_new_psw;
				int              ccode;
  	
				int              ready = 0;
				struct _lowcore *lc    = NULL;

				/*
				 * We shouldn't perform a TPI loop, waiting for
				 *  an interrupt to occur, but should load a
				 *  WAIT PSW instead. Otherwise we may keep the
				 *  channel subsystem busy, not able to present
				 *  the interrupt. When our sync. interrupt
				 *  arrived we reset the I/O old PSW to its
				 *  original value.
				 */
				memcpy( &io_new_psw,
				        &lc->io_new_psw,
				        sizeof(psw_t));

				ccode = iac();

				switch (ccode) {
				case 0:  		// primary-space
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_PRIM_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 1:			// secondary-space
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_SEC_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 2:			// access-register
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_ACC_REG_MODE
					                    | _PSW_IO_WAIT;
					break;
				case 3:			// home-space	
					io_sync_wait.mask =   _IO_PSW_MASK
					                    | _PSW_HOME_SPACE_MODE
					                    | _PSW_IO_WAIT;
					break;
				default:
					panic( "halt_IO() : unexpected "
					       "address-space-control %d\n",
					       ccode);
					break;
				} /* endswitch */

				io_sync_wait.addr = FIX_PSW(&&cio_wakeup);

				/*
				 * Martin didn't like modifying the new PSW, now we take
				 *  a fast exit in do_IRQ() instead
				 */
				*(__u32 *)__LC_SYNC_IO_WORD  = 1;

				do
				{

	      		asm volatile ( "lpsw %0" : : "m" (io_sync_wait) );
cio_wakeup:
				   io_parm = *(__u32 *)__LC_IO_INT_PARM;
				   io_sub  = (__u32)*(__u16 *)__LC_SUBCHANNEL_NR;

				   ready = s390_process_IRQ( io_sub );

				} while ( !((io_sub == irq) && (ready == 1)) );

				*(__u32 *)__LC_SYNC_IO_WORD = 0;

			} /* endif */

			ret = 0;
			break;

		case 1 :            /* status pending */
	
			ioinfo[irq]->devstat.flag |= DEVSTAT_STATUS_PENDING;

			/*
			 * initialize the device driver specific devstat irb area
			 */
			memset( &((devstat_t *) ioinfo[irq]->irq_desc.action->dev_id)->ii.irb,
			        '\0', sizeof( irb_t) );

			/*
			 * Let the common interrupt handler process the pending
			 *  status. However, we must avoid calling the user
			 *  action handler, as it won't be prepared to handle
                        *  a pending status during do_IO() processing inline.
			 *  This also implies that s390_process_IRQ must
			 *  terminate synchronously - especially if device
			 *  sensing is required.
			 */
			ioinfo[irq]->ui.flags.s_pend   = 1;
			ioinfo[irq]->ui.flags.busy     = 1;
			ioinfo[irq]->ui.flags.doio     = 1;

			s390_process_IRQ( irq );
			
			ioinfo[irq]->ui.flags.s_pend   = 0;
			ioinfo[irq]->ui.flags.busy     = 0;
			ioinfo[irq]->ui.flags.doio     = 0;
			ioinfo[irq]->ui.flags.repall   = 0;
			ioinfo[irq]->ui.flags.w4final  = 0;

			ioinfo[irq]->devstat.flag     |= DEVSTAT_FINAL_STATUS;

			/*
			 * In multipath mode a condition code 3 implies the last
			 *  path has gone, except we have previously restricted
			 *  the I/O to a particular path. A condition code 1
			 *  (0 won't occur) results in return code EIO as well
			 *  as 3 with another path than the one used (i.e. path available mask is non-zero).
			 */
			if ( ioinfo[irq]->devstat.ii.irb.scsw.cc == 3 )
			{
				ret                         = -ENODEV;
				ioinfo[irq]->devstat.flag  |= DEVSTAT_NOT_OPER;
				ioinfo[irq]->ui.flags.oper  = 0;
			}
			else
			{
				ret                         = -EIO;
				ioinfo[irq]->devstat.flag  &= ~DEVSTAT_NOT_OPER;
				ioinfo[irq]->ui.flags.oper  = 1;

			} /* endif */

			break;

		case 2 :            /* busy */

			ret = -EBUSY;
			break;

		default:            /* device not operational */

			ret = -ENODEV;
			break;

		} /* endswitch */

		if ( sync_isc_locked )
		{
			disable_cpu_sync_isc( irq );
  	
			sync_isc_locked              = 0;    // local setting
			ioinfo[irq]->ui.flags.syncio = 0;    // global setting
  	
			spin_unlock_irqrestore( &sync_isc, psw_flags);
  	
		} /* endif */

	} /* endif */

	return( ret );
}


/*
 * do_IRQ() handles all normal I/O device IRQ's (the special
 *          SMP cross-CPU interrupts have their own specific
 *          handlers).
 *
 * Returns: 0 - no ending status received, no further action taken
 *          1 - interrupt handler was called with ending status
 */
asmlinkage void do_IRQ( struct pt_regs regs,
                        unsigned int   irq,
                        __u32          s390_intparm )
{
#ifdef CONFIG_FAST_IRQ
	int			ccode;
	tpi_info_t 	tpi_info;
	int			new_irq;
#endif
	int			use_irq     = irq;

	//
	// fix me !!!
	//
	// We need to schedule device recognition, the interrupt stays
	//  pending. We need to dynamically allocate an ioinfo structure.
	//
	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return;	/* this keeps the device boxed ... */
	}

	/*
	 * take fast exit if CPU is in sync. I/O state
	 *
	 * Note: we have to turn off the WAIT bit and re-disable
	 *       interrupts prior to return as this was the initial
	 *       entry condition to synchronous I/O.
	 */
 	if (    *(__u32 *)__LC_SYNC_IO_WORD )
	{
		regs.psw.mask &= ~(_PSW_WAIT_MASK_BIT | _PSW_IO_MASK_BIT);

      return;

	} /* endif */

	s390irq_spin_lock(use_irq);

#ifdef CONFIG_FAST_IRQ
	do {
#endif /*  CONFIG_FAST_IRQ */

		s390_process_IRQ( use_irq );

#ifdef CONFIG_FAST_IRQ

		/*
		 * more interrupts pending ?
		 */
		ccode = tpi( &tpi_info );

		if ( ! ccode )
			break;  	// no, leave ...

		new_irq     = tpi_info.irq;

		/*
		 * if the interrupt is for a different irq we
		 *  release the current irq lock and obtain
		 *  a new one ...
		 */
		if ( new_irq != use_irq )
      {
			s390irq_spin_unlock(use_irq);
         use_irq = new_irq;
			s390irq_spin_lock(use_irq);

      } /* endif */

	} while ( 1 );

#endif /*  CONFIG_FAST_IRQ */

	s390irq_spin_unlock(use_irq);

	return;
}

/*
 * s390_process_IRQ() handles status pending situations and interrupts
 *
 * Called by : do_IRQ()             - for "real" interrupts
 *             s390_start_IO, halt_IO()
 *                                  - status pending cond. after SSCH, or HSCH
 *             disable_subchannel() - status pending conditions (after MSCH)
 *
 * Returns: 0 - no ending status received, no further action taken
 *          1 - interrupt handler was called with ending status
 */
int s390_process_IRQ( unsigned int irq )
{
	int                    ccode;      /* cond code from tsch() operation */
	int                    irb_cc;     /* cond code from irb */
	int                    sdevstat;   /* struct devstat size to copy */
	unsigned int           fctl;       /* function control */
	unsigned int           stctl;      /* status   control */
	unsigned int           actl;       /* activity control */
	struct s390_irqaction *action;
	struct pt_regs         regs;       /* for interface compatibility only */

	int               issense         = 0;
	int               ending_status   = 0;
	int               allow4handler   = 1;
	int               chnchk          = 0;
#if 0
	int               cpu             = smp_processor_id();

	kstat.irqs[cpu][irq]++;
#endif

	if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		/* we can't properly process the interrupt ... */
		tsch( irq, &init_irb );
		return( 1 );
	}
	else
	{
		action = ioinfo[irq]->irq_desc.action;
	
  	} /* endif */

#ifdef CONFIG_DEBUG_IO
	/*
	 * It might be possible that a device was not-oper. at the time
	 *  of free_irq() processing. This means the handler is no longer
	 *  available when the device possibly becomes ready again. In
	 *  this case we perform delayed disable_subchannel() processing.
	 */
	if ( action == NULL )
	{
		if ( !ioinfo[irq]->ui.flags.d_disable )
		{
			printk( KERN_CRIT"s390_process_IRQ(%04X) "
			        "- no interrupt handler registered "
					  "for device %04X !\n",
			        irq,
			        ioinfo[irq]->devstat.devno);

		} /* endif */

	} /* endif */
#endif

	/*
	 * retrieve the i/o interrupt information (irb),
	 *  update the device specific status information
	 *  and possibly call the interrupt handler.
	 *
	 * Note 1: At this time we don't process the resulting
	 *         condition code (ccode) from tsch(), although
	 *         we probably should.
	 *
	 * Note 2: Here we will have to check for channel
	 *         check conditions and call a channel check
	 *         handler.
	 *
	 * Note 3: If a start function was issued, the interruption
	 *         parameter relates to it. If a halt function was
	 *         issued for an idle device, the intparm must not
	 *         be taken from lowcore, but from the devstat area.
	 */
	ccode = tsch( irq, &(ioinfo[irq]->devstat.ii.irb) );

	//
	// We must only accumulate the status if the device is busy already
	//
	if ( ioinfo[irq]->ui.flags.busy )
	{
		ioinfo[irq]->devstat.dstat   |= ioinfo[irq]->devstat.ii.irb.scsw.dstat;
		ioinfo[irq]->devstat.cstat   |= ioinfo[irq]->devstat.ii.irb.scsw.cstat;
                ioinfo[irq]->devstat.intparm  = ioinfo[irq]->u_intparm; 
                                                                        
	}
	else
	{
		ioinfo[irq]->devstat.dstat    = ioinfo[irq]->devstat.ii.irb.scsw.dstat;
		ioinfo[irq]->devstat.cstat    = ioinfo[irq]->devstat.ii.irb.scsw.cstat;

		ioinfo[irq]->devstat.flag     = 0;   // reset status flags
		ioinfo[irq]->devstat.intparm  = 0; 
                                                                        
	} /* endif */

	ioinfo[irq]->devstat.lpum = ioinfo[irq]->devstat.ii.irb.esw.esw1.lpum;

	/*
	 * reset device-busy bit if no longer set in irb
	 */
	if (   (ioinfo[irq]->devstat.dstat & DEV_STAT_BUSY                   )
	    && ((ioinfo[irq]->devstat.ii.irb.scsw.dstat & DEV_STAT_BUSY) == 0))
	{
		ioinfo[irq]->devstat.dstat &= ~DEV_STAT_BUSY;

	} /* endif */

	/*
	 * Save residual count and CCW information in case primary and
	 *  secondary status are presented with different interrupts.
	 */
	if ( ioinfo[irq]->devstat.ii.irb.scsw.stctl
	           & (   SCSW_STCTL_PRIM_STATUS | SCSW_STCTL_INTER_STATUS ) )
	{
		ioinfo[irq]->devstat.rescnt = ioinfo[irq]->devstat.ii.irb.scsw.count;
		ioinfo[irq]->devstat.cpa    = ioinfo[irq]->devstat.ii.irb.scsw.cpa;

#ifdef CONFIG_DEBUG_IO
		if ( irq != cons_dev )
			printk( "s390_process_IRQ( %04X ) : "
			        "residual count from irb after tsch() %d\n",
			        irq, ioinfo[irq]->devstat.rescnt );
#endif

	} /* endif */

	irb_cc = ioinfo[irq]->devstat.ii.irb.scsw.cc;

	//
	// check for any kind of channel or interface control check but don't
	//  issue the message for the console device
	//
	if (    (ioinfo[irq]->devstat.ii.irb.scsw.cstat
	            & (  SCHN_STAT_CHN_DATA_CHK
	               | SCHN_STAT_CHN_CTRL_CHK
	               | SCHN_STAT_INTF_CTRL_CHK )       )
	     && (irq != cons_dev                         ) )
	{
		printk( "Channel-Check or Interface-Control-Check "
		        "received\n"
		        " ... device %04X on subchannel %04X, dev_stat "
		        ": %02X sch_stat : %02X\n",
		        ioinfo[irq]->devstat.devno,
		        irq,
		        ioinfo[irq]->devstat.dstat,
		        ioinfo[irq]->devstat.cstat);

		chnchk = 1;

	} /* endif */

	issense = ioinfo[irq]->devstat.ii.irb.esw.esw0.erw.cons;

	if ( issense )
	{
		ioinfo[irq]->devstat.scnt  =
		             ioinfo[irq]->devstat.ii.irb.esw.esw0.erw.scnt;
		ioinfo[irq]->devstat.flag |=
		             DEVSTAT_FLAG_SENSE_AVAIL;
                  	
		sdevstat = sizeof( devstat_t);

#ifdef CONFIG_DEBUG_IO
		if ( irq != cons_dev )
			printk( "s390_process_IRQ( %04X ) : "
			        "concurrent sense bytes avail %d\n",
			        irq, ioinfo[irq]->devstat.scnt );
#endif
	}
	else
	{
		/* don't copy the sense data area ! */
		sdevstat = sizeof( devstat_t) - SENSE_MAX_COUNT;

	} /* endif */

	switch ( irb_cc ) {
	case 1:      /* status pending */

		ioinfo[irq]->devstat.flag |= DEVSTAT_STATUS_PENDING;

	case 0:      /* normal i/o interruption */

		fctl  = ioinfo[irq]->devstat.ii.irb.scsw.fctl;
		stctl = ioinfo[irq]->devstat.ii.irb.scsw.stctl;
		actl  = ioinfo[irq]->devstat.ii.irb.scsw.actl;

		if ( chnchk && (ioinfo[irq]->senseid.cu_type == 0x3088))
		{
			char buffer[80];
   	
			sprintf( buffer, "s390_process_IRQ(%04X) - irb for "
			         "device %04X after channel check\n",
			         irq,
			         ioinfo[irq]->devstat.devno );

			s390_displayhex( buffer,
			                 &(ioinfo[irq]->devstat.ii.irb) ,
			                 sizeof(irb_t));
		} /* endif */
			
		ioinfo[irq]->stctl |= stctl;

		ending_status =    ( stctl & SCSW_STCTL_SEC_STATUS                          )
			|| ( stctl == (SCSW_STCTL_ALERT_STATUS | SCSW_STCTL_STATUS_PEND)         )
		   || ( (fctl == SCSW_FCTL_HALT_FUNC)  && (stctl == SCSW_STCTL_STATUS_PEND) )
		   || ( (fctl == SCSW_FCTL_CLEAR_FUNC) && (stctl == SCSW_STCTL_STATUS_PEND) );

		/*
		 * Check for unsolicited interrupts - for debug purposes only
		 *
		 * We only consider an interrupt as unsolicited, if the device was not
		 *  actively in use (busy) and an interrupt other than an ALERT status
		 *  was received.
		 *
		 * Note: We must not issue a message to the console, if the
		 *       unsolicited interrupt applies to the console device
		 *       itself !
		 */
#ifdef CONFIG_DEBUG_IO
		if (     ( irq != cons_dev                 )
			 && !( stctl & SCSW_STCTL_ALERT_STATUS )
			 &&  ( ioinfo[irq]->ui.flags.busy == 0  ) )
		{
	      char buffer[80];

			printk( "Unsolicited interrupt received for device %04X on subchannel %04X\n"
				" ... device status : %02X subchannel status : %02X\n",
				ioinfo[irq]->devstat.devno,
				irq,
				ioinfo[irq]->devstat.dstat,
				ioinfo[irq]->devstat.cstat);

   	   sprintf( buffer, "s390_process_IRQ(%04X) - irb for "
			         "device %04X, ending_status %d\n",
			         irq,
			         ioinfo[irq]->devstat.devno,
		   	      ending_status);

			s390_displayhex( buffer,
			                 &(ioinfo[irq]->devstat.ii.irb) ,
			                 sizeof(irb_t));

		} /* endif */

#endif
		/*
		 * take fast exit if no handler is available
		 */
		if ( !action )
			return( ending_status );     		

		/*
		 * Check whether we must issue a SENSE CCW ourselves if there is no
		 *  concurrent sense facility installed for the subchannel.
		 *
		 * Note: We should check for ioinfo[irq]->ui.flags.consns but VM
		 *       violates the ESA/390 architecture and doesn't present an
		 *       operand exception for virtual devices without concurrent
		 *       sense facility available/supported when enabling the
       *       concurrent sense facility.
		 */
		if (    (    ( ioinfo[irq]->devstat.ii.irb.scsw.dstat & DEV_STAT_UNIT_CHECK )
			       && ( !issense                                                    ) )
           || ( ioinfo[irq]->ui.flags.delsense && ending_status                     ) )
		{
			int            ret_io;
			ccw1_t        *s_ccw  = &ioinfo[irq]->senseccw;
			unsigned long  s_flag = 0;

         if ( ending_status )
         {
				/*
				 * We copy the current status information into the device driver
				 *  status area. Then we can use the local devstat area for device
				 *  sensing. When finally calling the IRQ handler we must not overlay
				 *  the original device status but copy the sense data only.
				 */
				memcpy( action->dev_id,
				        &(ioinfo[irq]->devstat),
				        sizeof( devstat_t) );

				s_ccw->cmd_code = CCW_CMD_BASIC_SENSE;
				s_ccw->cda      = (__u32)virt_to_phys( ioinfo[irq]->sense_data );
				s_ccw->count    = SENSE_MAX_COUNT;
				s_ccw->flags    = CCW_FLAG_SLI;

				/*
				 * If free_irq() or a sync do_IO/s390_start_IO() is in
				 *  process we have to sense synchronously
				 */
				if ( ioinfo[irq]->ui.flags.unready || ioinfo[irq]->ui.flags.syncio )
				{
					s_flag = DOIO_WAIT_FOR_INTERRUPT;

				} /* endif */

				/*
				 * Reset status info
				 *
				 * It does not matter whether this is a sync. or async.
				 *  SENSE request, but we have to assure we don't call
				 *  the irq handler now, but keep the irq in busy state.
				 *  In sync. mode s390_process_IRQ() is called recursively,
				 *  while in async. mode we re-enter do_IRQ() with the
				 *  next interrupt.
				 *
				 * Note : this may be a delayed sense request !
				 */
				allow4handler                  = 0;

				ioinfo[irq]->ui.flags.fast     = 0;
				ioinfo[irq]->ui.flags.repall   = 0;
				ioinfo[irq]->ui.flags.w4final  = 0;
				ioinfo[irq]->ui.flags.delsense = 0;

				ioinfo[irq]->devstat.cstat     = 0;
				ioinfo[irq]->devstat.dstat     = 0;
				ioinfo[irq]->devstat.rescnt    = SENSE_MAX_COUNT;

				ioinfo[irq]->ui.flags.w4sense  = 1;
			
				ret_io = s390_start_IO( irq,
				                        s_ccw,
				                        0xE2C5D5E2,  // = SENSe
				                        0,           // n/a
				                        s_flag);
			}
			else
			{
				/*
				 * we received an Unit Check but we have no final
				 *  status yet, therefore we must delay the SENSE
				 *  processing. However, we must not report this
				 *  intermediate status to the device interrupt
				 *  handler.
				 */
				ioinfo[irq]->ui.flags.fast     = 0;
				ioinfo[irq]->ui.flags.repall   = 0;

				ioinfo[irq]->ui.flags.delsense = 1;
				allow4handler                  = 0;

			} /* endif */

		} /* endif */

		/*
		 * we allow for the device action handler if .
		 *  - we received ending status
		 *  - the action handler requested to see all interrupts
		 *  - we received an intermediate status
		 *  - fast notification was requested (primary status)
		 *  - unsollicited interrupts
		 *
		 */
		if ( allow4handler )
		{
			allow4handler =    ending_status
			   || ( ioinfo[irq]->ui.flags.repall                                      )
			   || ( stctl & SCSW_STCTL_INTER_STATUS                                   )
				|| ( (ioinfo[irq]->ui.flags.fast ) && (stctl & SCSW_STCTL_PRIM_STATUS) )
				|| ( ioinfo[irq]->ui.flags.oper == 0                                   );

		} /* endif */

		/*
		 * We used to copy the device status information right before
		 *  calling the device action handler. However, in status
		 *  pending situations during do_IO() or halt_IO(), as well as
		 *  enable_subchannel/disable_subchannel processing we must
		 *  synchronously return the status information and must not
		 *  call the device action handler.
		 *
		 */
		if ( allow4handler )
		{
			/*
			 * if we were waiting for sense data we copy the sense
			 *  bytes only as the original status information was
			 *  saved prior to sense already.
			 */
			if ( ioinfo[irq]->ui.flags.w4sense )
			{
				int sense_count = SENSE_MAX_COUNT-ioinfo[irq]->devstat.rescnt;

#ifdef CONFIG_DEBUG_IO
      if ( irq != cons_dev )
         printk( "s390_process_IRQ( %04X ) : "
                 "BASIC SENSE bytes avail %d\n",
                 irq, sense_count );
#endif
				ioinfo[irq]->ui.flags.w4sense          = 0;
				((devstat_t *)(action->dev_id))->flag |= DEVSTAT_FLAG_SENSE_AVAIL;
				((devstat_t *)(action->dev_id))->scnt  = sense_count;

				if ( sense_count >= 0 )
				{
					memcpy( ((devstat_t *)(action->dev_id))->ii.sense.data,
					        &(ioinfo[irq]->sense_data),
					        sense_count);
				}
				else
				{
#if 1
					panic( "s390_process_IRQ(%04x) encountered "
					       "negative sense count\n",
					       irq);
#else
					printk( KERN_CRIT"s390_process_IRQ(%04x) encountered "
					        "negative sense count\n",
					        irq);
#endif
				} /* endif */
			}
			else
			{
				memcpy( action->dev_id, &(ioinfo[irq]->devstat), sdevstat );

			}  /* endif */

      } /* endif */

		/*
		 * for status pending situations other than deferred interrupt
		 *  conditions detected by s390_process_IRQ() itself we must not
		 *  call the handler. This will synchronously be reported back
		 *  to the caller instead, e.g. when detected during do_IO().
		 */
		if (    ioinfo[irq]->ui.flags.s_pend
		     || ioinfo[irq]->ui.flags.unready
		     || ioinfo[irq]->ui.flags.repnone )
		{		
			if ( ending_status )
			{

				ioinfo[irq]->ui.flags.busy     = 0;
				ioinfo[irq]->ui.flags.doio     = 0;
				ioinfo[irq]->ui.flags.haltio   = 0;
				ioinfo[irq]->ui.flags.fast     = 0;
				ioinfo[irq]->ui.flags.repall   = 0;
				ioinfo[irq]->ui.flags.w4final  = 0;

				ioinfo[irq]->devstat.flag     |= DEVSTAT_FINAL_STATUS;
				action->dev_id->flag          |= DEVSTAT_FINAL_STATUS;

			} /* endif */

			allow4handler = 0;

		} /* endif */

		/*
		 * Call device action handler if applicable
		 */
		if ( allow4handler )
		{

			/*
			 *  We only reset the busy condition when we are sure that no further
			 *   interrupt is pending for the current I/O request (ending_status).
			 */
			if ( ending_status || !ioinfo[irq]->ui.flags.oper )
			{
				ioinfo[irq]->ui.flags.oper     = 1;  /* dev IS oper */

				ioinfo[irq]->ui.flags.busy     = 0;
				ioinfo[irq]->ui.flags.doio     = 0;
				ioinfo[irq]->ui.flags.haltio   = 0;
				ioinfo[irq]->ui.flags.fast     = 0;
				ioinfo[irq]->ui.flags.repall   = 0;
				ioinfo[irq]->ui.flags.w4final  = 0;

				ioinfo[irq]->devstat.flag             |= DEVSTAT_FINAL_STATUS;
				((devstat_t *)(action->dev_id))->flag |= DEVSTAT_FINAL_STATUS;

				action->handler( irq, action->dev_id, &regs );

				//
				// reset intparm after final status or we will badly present unsolicited
				//  interrupts with a intparm value possibly no longer valid.
				//
				ioinfo[irq]->devstat.intparm   = 0;

				//
				// Was there anything queued ? Start the pending channel program
				//  if there is one.
				//
				if ( ioinfo[irq]->ui.flags.doio_q )
				{
					int ret;

					ret = s390_start_IO( irq,
							               ioinfo[irq]->qcpa,
							               ioinfo[irq]->qintparm,
							               ioinfo[irq]->qlpm,
							               ioinfo[irq]->qflag);

					ioinfo[irq]->ui.flags.doio_q = 0;

					/*
					 * If s390_start_IO() failed call the device's interrupt
					 *  handler, the IRQ related devstat area was setup by
					 *  s390_start_IO() accordingly already (status pending
					 *  condition).
					 */
					if ( ret )
					{
						action->handler( irq, action->dev_id, &regs );

					} /* endif */

				} /* endif */

			}
			else
			{
				ioinfo[irq]->ui.flags.w4final = 1;

				/*
				 * Eventually reset subchannel PCI status and
				 *  set the PCI or SUSPENDED flag in the user
				 *  device status block if appropriate.
				 */
				if ( ioinfo[irq]->devstat.cstat & SCHN_STAT_PCI )
				{
					((devstat_t *)(action->dev_id))->flag |= DEVSTAT_PCI;
					ioinfo[irq]->devstat.cstat &= ~SCHN_STAT_PCI;
				}

				if ( actl & SCSW_ACTL_SUSPENDED )
				{
					((devstat_t *)(action->dev_id))->flag |= DEVSTAT_SUSPENDED;

				} /* endif */

				action->handler( irq, action->dev_id, &regs );

			} /* endif */

		} /* endif */

		break;

	case 3:      /* device/path not operational */

		ioinfo[irq]->ui.flags.busy    = 0;
		ioinfo[irq]->ui.flags.doio    = 0;
		ioinfo[irq]->ui.flags.haltio  = 0;

		ioinfo[irq]->devstat.cstat    = 0;
		ioinfo[irq]->devstat.dstat    = 0;

		if ( ioinfo[irq]->ulpm != ioinfo[irq]->opm )
		{
			/*
			 * either it was the only path or it was restricted ...
			 */
			ioinfo[irq]->opm &= ~(ioinfo[irq]->devstat.ii.irb.esw.esw1.lpum);
		}
		else
		{
			ioinfo[irq]->opm = 0;

		} /* endif */
	
		if ( ioinfo[irq]->opm == 0 ) 	
		{
			ioinfo[irq]->ui.flags.oper  = 0;

		} /* endif */

		ioinfo[irq]->devstat.flag |= DEVSTAT_NOT_OPER;
		ioinfo[irq]->devstat.flag |= DEVSTAT_FINAL_STATUS;

		/*
		 * When we find a device "not oper" we save the status
		 *  information into the device status area and call the
		 *  device specific interrupt handler.
		 *
		 * Note: currently we don't have any way to reenable
		 *       the device unless an unsolicited interrupt
		 *       is presented. We don't check for spurious
		 *       interrupts on "not oper" conditions.
		 */

		if (    ( ioinfo[irq]->ui.flags.fast    )
		     && ( ioinfo[irq]->ui.flags.w4final ) )
		{
			/*
			 * If a new request was queued already, we have
			 *  to simulate the "not oper" status for the
			 *  queued request by switching the "intparm" value
			 *  and notify the interrupt handler.
			 */
			if ( ioinfo[irq]->ui.flags.doio_q )
			{
				ioinfo[irq]->devstat.intparm = ioinfo[irq]->qintparm;

			} /* endif */

		} /* endif */

		ioinfo[irq]->ui.flags.fast    = 0;
		ioinfo[irq]->ui.flags.repall  = 0;
		ioinfo[irq]->ui.flags.w4final = 0;

		/*
		 * take fast exit if no handler is available
		 */
		if ( !action )
			return( ending_status );     		

		memcpy( action->dev_id, &(ioinfo[irq]->devstat), sdevstat );

		ioinfo[irq]->devstat.intparm  = 0;

		if ( !ioinfo[irq]->ui.flags.s_pend )
		{
			action->handler( irq, action->dev_id, &regs );

		} /* endif */

		ending_status    = 1;

		break;

	} /* endswitch */

	return( ending_status );
}

/*
 * Set the special i/o-interruption sublass 7 for the
 *  device specified by parameter irq. There can only
 *  be a single device been operated on this special
 *  isc. This function is aimed being able to check
 *  on special device interrupts in disabled state,
 *  without having to delay I/O processing (by queueing)
 *  for non-console devices.
 *
 * Setting of this isc is done by set_cons_dev(), while
 *  reset_cons_dev() resets this isc and re-enables the
 *  default isc3 for this device. wait_cons_dev() allows
 *  to actively wait on an interrupt for this device in
 *  disabed state. When the interrupt condition is
 *  encountered, wait_cons_dev(9 calls do_IRQ() to have
 *  the console device driver processing the interrupt.
 */
int set_cons_dev( int irq )
{
	int           ccode;
	unsigned long cr6 __attribute__ ((aligned (8)));
	int           rc = 0;

	if ( cons_dev != -1  )
	{
		rc = -EBUSY;
	}
	else if ( (irq > highest_subchannel) || (irq < 0) )
	{
		rc = -ENODEV;
	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }
	else
	{
		/*
		 * modify the indicated console device to operate
		 *  on special console interrupt sublass 7
		 */
		ccode = stsch( irq, &(ioinfo[irq]->schib) );

		if (ccode)
		{
			rc                         = -ENODEV;
			ioinfo[irq]->devstat.flag |= DEVSTAT_NOT_OPER;
		}
		else
		{
			ioinfo[irq]->schib.pmcw.isc = 7;

			ccode = msch( irq, &(ioinfo[irq]->schib) );

			if (ccode)
			{
				rc = -EIO;
			}
			else
			{
				cons_dev = irq;

				/*
				 * enable console I/O-interrupt sublass 7
				 */
				asm volatile ("STCTL 6,6,%0": "=m" (cr6));
				cr6 |= 0x01000000;
				asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

			} /* endif */

		} /* endif */

	} /* endif */

	return( rc);
}

int reset_cons_dev( int irq)
{
	int     rc = 0;
	int     ccode;
	long    cr6 __attribute__ ((aligned (8)));

	if ( cons_dev != -1  )
	{
		rc = -EBUSY;
	}
	else if ( (irq > highest_subchannel) || (irq < 0) )
	{
		rc = -ENODEV;
	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }
	else
	{
		/*
		 * reset the indicated console device to operate
		 *  on default console interrupt sublass 3
		 */
		ccode = stsch( irq, &(ioinfo[irq]->schib) );

		if (ccode)
		{
			rc                         = -ENODEV;
			ioinfo[irq]->devstat.flag |= DEVSTAT_NOT_OPER;
		}
		else
		{

			ioinfo[irq]->schib.pmcw.isc = 3;

			ccode = msch( irq, &(ioinfo[irq]->schib) );

			if (ccode)
			{
				rc = -EIO;
			}
			else
			{
				cons_dev = -1;

				/*
				 * disable special console I/O-interrupt sublass 7
				 */
				asm volatile ("STCTL 6,6,%0": "=m" (cr6));
				cr6 &= 0xFEFFFFFF;
				asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

			} /* endif */

		} /* endif */

	} /* endif */

	return( rc);
}

int wait_cons_dev( int irq )
{
	int              rc = 0;
	long             save_cr6;

	if ( irq == cons_dev )
	{

		/*
		 * before entering the spinlock we may already have
		 *  processed the interrupt on a different CPU ...
		 */
		if ( ioinfo[irq]->ui.flags.busy == 1 )
		{
			long cr6 __attribute__ ((aligned (8)));

			/*
			 * disable all, but isc 7 (console device)
			 */
			asm volatile ("STCTL 6,6,%0": "=m" (cr6));
			save_cr6  = cr6;
			cr6      &= 0x01FFFFFF;
			asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

			do {
				tpi_info_t tpi_info;
				if (tpi(&tpi_info) == 1) {
					s390_process_IRQ( tpi_info.irq );
				} else {
					s390irq_spin_unlock(irq);
					tod_wait(100);
					s390irq_spin_lock(irq);
				}
				eieio();
			} while (ioinfo[irq]->ui.flags.busy == 1);

			/*
			 * restore previous isc value
			 */
			asm volatile ("STCTL 6,6,%0": "=m" (cr6));
			cr6 = save_cr6;
			asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");

		} /* endif */

	}
	else
	{
		rc = EINVAL;

	} /* endif */


	return(rc);
}


int enable_cpu_sync_isc( int irq )
{
	int             ccode;
	long            cr6 __attribute__ ((aligned (8)));

	int             count = 0;
	int             rc    = 0;

	if ( irq <= highest_subchannel && ioinfo[irq] != INVALID_STORAGE_AREA )
	{
		ccode = stsch( irq, &(ioinfo[irq]->schib) );

		if ( !ccode )
		{
			ioinfo[irq]->schib.pmcw.isc = 5;

			do
			{
				ccode = msch( irq, &(ioinfo[irq]->schib) );

				if (ccode == 0 )
				{
					/*
					 * enable interrupt subclass in CPU
					 */
					asm volatile ("STCTL 6,6,%0": "=m" (cr6));
					cr6 |= 0x04000000;  // enable sync isc 5
					cr6 &= 0xEFFFFFFF;  // disable standard isc 3
					asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");
				}
				else if (ccode == 3)
				{
					rc = -ENODEV;  // device not-oper - very unlikely

				}
				else if (ccode == 2)
				{
					rc = -EBUSY;   // device busy - should not happen

				}
				else if (ccode == 1)
				{
					//
					// process pending status
					//
					ioinfo[irq]->ui.flags.s_pend = 1;

					s390_process_IRQ( irq );

					ioinfo[irq]->ui.flags.s_pend = 0;

					count++;

				} /* endif */

			} while ( ccode == 1 && count < 3 );

			if ( count == 3)
			{
				rc = -EIO;

			} /* endif */
		}
		else
		{
			rc = -ENODEV;     // device is not-operational

		} /* endif */
	}
	else
	{
		rc = -EINVAL;

	} /* endif */

	return( rc);
}

int disable_cpu_sync_isc( int irq)
{
	int     rc    = 0;
	int     retry = 5;
	int     ccode;
	long    cr6 __attribute__ ((aligned (8)));

	if ( irq <= highest_subchannel && ioinfo[irq] != INVALID_STORAGE_AREA )
	{
		ccode = stsch( irq, &(ioinfo[irq]->schib) );

		ioinfo[irq]->schib.pmcw.isc = 3;

		do {

			ccode = msch( irq, &(ioinfo[irq]->schib) );

			switch ( ccode ) {
			case 0:
				/*
				 * disable interrupt subclass in CPU
				 */
				asm volatile ("STCTL 6,6,%0": "=m" (cr6));
				cr6 &= 0xFBFFFFFF; // disable sync isc 5
				cr6 |= 0x10000000; // enable standard isc 3
				asm volatile ("LCTL 6,6,%0":: "m" (cr6):"memory");
				break;
			case 1:
				ioinfo[irq]->ui.flags.s_pend = 1;
				s390_process_IRQ( irq );
				ioinfo[irq]->ui.flags.s_pend = 0;
				retry--;
				rc = -EIO;
				break;
			case 2:
				rc = -EBUSY;
				break;
			default:
				rc = -ENODEV;
				break;
			} /* endswitch */

		} while ( retry && (ccode ==1) );
	}
	else
	{
		rc = -EINVAL;

	} /* endif */

	return( rc);
}

//
// Input :
//   devno - device number
//   ps    - pointer to sense ID data area
//
// Output : none
//
void VM_virtual_device_info( unsigned int devno,
                             senseid_t *ps )
{
	diag210_t  diag_data;
	int        ccode;

	int        error = 0;

	diag_data.vrdcdvno = devno;
	diag_data.vrdclen  = sizeof( diag210_t);
	ccode              = diag210( (diag210_t *)virt_to_phys( &diag_data ) );
	ps->reserved       = 0xff;

	switch (diag_data.vrdcvcla) {
	case 0x80:

		switch (diag_data.vrdcvtyp) {
		case 00:

			ps->cu_type   = 0x3215;

			break;

		default:

			error = 1;

			break;

		} /* endswitch */

		break;

	case 0x40:

		switch (diag_data.vrdcvtyp) {
		case 0xC0:

			ps->cu_type   = 0x5080;

			break;

		case 0x80:

			ps->cu_type   = 0x2250;

			break;

		case 0x04:

			ps->cu_type   = 0x3277;

			break;

		case 0x01:

			ps->cu_type   = 0x3278;

			break;

		default:

			error = 1;

			break;

		} /* endswitch */

		break;

	case 0x20:

		switch (diag_data.vrdcvtyp) {
		case 0x84:

			ps->cu_type   = 0x3505;

			break;

		case 0x82:

			ps->cu_type   = 0x2540;

			break;

		case 0x81:

			ps->cu_type   = 0x2501;

			break;

		default:

			error = 1;

			break;

		} /* endswitch */

		break;

	case 0x10:

		switch (diag_data.vrdcvtyp) {
		case 0x84:

			ps->cu_type   = 0x3525;

			break;

		case 0x82:

			ps->cu_type   = 0x2540;

			break;

		case 0x4F:
		case 0x4E:
		case 0x48:

			ps->cu_type   = 0x3820;

			break;

		case 0x4D:
		case 0x49:
		case 0x45:

			ps->cu_type   = 0x3800;

			break;

		case 0x4B:

			ps->cu_type   = 0x4248;

			break;

		case 0x4A:

			ps->cu_type   = 0x4245;

			break;

		case 0x47:

			ps->cu_type   = 0x3262;

			break;

		case 0x43:

			ps->cu_type   = 0x3203;

			break;

		case 0x42:

			ps->cu_type   = 0x3211;

			break;

		case 0x41:

			ps->cu_type   = 0x1403;

			break;

		default:

			error = 1;

			break;

		} /* endswitch */

		break;

	case 0x08:

		switch (diag_data.vrdcvtyp) {
		case 0x82:

			ps->cu_type   = 0x3422;

			break;

		case 0x81:

			ps->cu_type   = 0x3490;

			break;

		case 0x10:

			ps->cu_type   = 0x3420;

			break;

		case 0x02:

			ps->cu_type   = 0x3430;

			break;

		case 0x01:

			ps->cu_type   = 0x3480;

			break;

		case 0x42:

			ps->cu_type   = 0x3424;

			break;

		case 0x44:

			ps->cu_type   = 0x9348;

			break;

		default:

			error = 1;

			break;

		} /* endswitch */

		break;

	case 02: /* special device class ... */

		switch (diag_data.vrdcvtyp) {
		case 0x20: /* OSA */

			ps->cu_type   = 0x3088;
			ps->cu_model  = 0x60;

			break;

		default:

			error = 1;
      	break;

		} /* endswitch */

		break;

	default:

		error = 1;

		break;

	} /* endswitch */

	if ( error )
	{printk( "DIAG X'210' for device %04X returned (cc = %d): vdev class : %02X, "
			"vdev type : %04X \n ...  rdev class : %02X, rdev type : %04X, rdev model: %02X\n",
			devno,
			ccode,
			diag_data.vrdcvcla,
			diag_data.vrdcvtyp,
			diag_data.vrdcrccl,
			diag_data.vrdccrty,
			diag_data.vrdccrmd );

	} /* endif */

}

/*
 * This routine returns the characteristics for the device
 *  specified. Some old devices might not provide the necessary
 *  command code information during SenseID processing. In this
 *  case the function returns -EINVAL. Otherwise the function
 *  allocates a decice specific data buffer and provides the
 *  device characteristics together with the buffer size. Its
 *  the callers responability to release the kernel memory if
 *  not longer needed. In case of persistent I/O problems -EBUSY
 *  is returned.
 *
 *  The function may be called enabled or disabled. However, the
 *   caller must have locked the irq it is requesting data for.
 *
 * Note : It would have been nice to collect this information
 *         during init_IRQ() processing but this is not possible
 *
 *         a) without statically pre-allocation fixed size buffers
 *            as virtual memory management isn't available yet.
 *
 *         b) without unnecessarily increase system startup by
 *            evaluating devices eventually not used at all.
 */
int read_dev_chars( int irq, void **buffer, int length )
{
	unsigned int  flags;
	ccw1_t       *rdc_ccw;
	devstat_t     devstat;
	char         *rdc_buf;
	int           devflag;

	int           ret      = 0;
	int           emulated = 0;
	int           retry    = 5;

	if ( !buffer || !length )
	{
		return( -EINVAL );

	} /* endif */

	if ( (irq > highest_subchannel) || (irq < 0 ) )
	{
	return( -ENODEV );

	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
   }

	if ( ioinfo[irq]->ui.flags.oper == 0 )
	{
		return( -ENODEV );

	} /* endif */

	/*
	 * Before playing around with irq locks we should assure
	 *   running disabled on (just) our CPU. Sync. I/O requests
    *   also require to run disabled.
	 *
	 * Note : as no global lock is required, we must not use
	 *        cli(), but __cli() instead.  	
	 */
	__save_flags(flags);
	__cli();

	rdc_ccw = &ioinfo[irq]->senseccw;

	if ( !ioinfo[irq]->ui.flags.ready )
	{
		ret = request_irq( irq,
		                   init_IRQ_handler,
		                   0, "RDC", &devstat );

		if ( !ret )
		{
			emulated = 1;

		} /* endif */

	} /* endif */

	if ( !ret )
	{
		if ( ! *buffer )
		{
			rdc_buf  = kmalloc( length, GFP_KERNEL);
		}
		else
		{
			rdc_buf = *buffer;

		} /* endif */

		if ( !rdc_buf )
		{
			ret = -ENOMEM;
		}
		else
		{
			do
			{
				rdc_ccw->cmd_code = CCW_CMD_RDC;
				rdc_ccw->cda      = (__u32)virt_to_phys( rdc_buf );
				rdc_ccw->count    = length;
				rdc_ccw->flags    = CCW_FLAG_SLI;

				memset( (devstat_t *)(ioinfo[irq]->irq_desc.action->dev_id),
				        '\0',
				        sizeof( devstat_t));

				ret = s390_start_IO( irq,
				                     rdc_ccw,
				                     0x00524443, // RDC
				                     0,          // n/a
				                     DOIO_WAIT_FOR_INTERRUPT
				                      | DOIO_DONT_CALL_INTHDLR );
				retry--;
				devflag = ((devstat_t *)(ioinfo[irq]->irq_desc.action->dev_id))->flag;

			} while (    ( retry                                     )
			          && ( ret || (devflag & DEVSTAT_STATUS_PENDING) ) );

		} /* endif */

		if ( !retry )
		{
			ret = -EBUSY;

		} /* endif */

		__restore_flags(flags);

		/*
		 * on success we update the user input parms
		 */
		if ( !ret )
		{
			*buffer = rdc_buf;

		} /* endif */

		if ( emulated )
		{
			free_irq( irq, &devstat);

		} /* endif */

	} /* endif */

	return( ret );
}

/*
 *  Read Configuration data
 */
int read_conf_data( int irq, void **buffer, int *length, __u8 lpm )
{
	unsigned long flags;
	int           ciw_cnt;

	int           found  = 0; // RCD CIW found
	int           ret    = 0; // return code

	if ( (irq > highest_subchannel) || (irq < 0 ) )
	{
		return( -ENODEV );
	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
	}
	else if ( !buffer || !length )
	{
		return( -EINVAL);
	}
	else if ( ioinfo[irq]->ui.flags.oper == 0 )
	{
		return( -ENODEV );
	}
	else if ( ioinfo[irq]->ui.flags.esid == 0 )
	{
		return( -EOPNOTSUPP );

	} /* endif */

	/*
	 * scan for RCD command in extended SenseID data
	 */
	
	for ( ciw_cnt = 0; (found == 0) && (ciw_cnt < 62); ciw_cnt++ )
	{
		if ( ioinfo[irq]->senseid.ciw[ciw_cnt].ct == CIW_TYPE_RCD )
		{
			/*
			 * paranoia check ...
			 */
			if ( ioinfo[irq]->senseid.ciw[ciw_cnt].cmd != 0 )
			{
				found = 1;

			} /* endif */

      	break;

      } /* endif */

   } /* endfor */

	if ( found )
	{
		devstat_t  devstat;  /* inline device status area */
		devstat_t *pdevstat;
		int        ioflags;

		ccw1_t    *rcd_ccw  = &ioinfo[irq]->senseccw;
		char      *rcd_buf  = NULL;
		int        emulated = 0; /* no i/O handler installed */
		int        retry    = 5; /* retry count */

		__save_flags(flags);
		__cli();

		if ( !ioinfo[irq]->ui.flags.ready )
		{
			pdevstat = &devstat;
			ret      = request_irq( irq,
			                        init_IRQ_handler,
			                        0, "RCD", pdevstat );

			if ( !ret )
			{
				emulated = 1;

			} /* endif */
		}
		else
		{
			pdevstat = ioinfo[irq]->irq_desc.action->dev_id;

		} /* endif */

		if ( !ret )
		{
			if ( init_IRQ_complete )
			{
				rcd_buf = kmalloc( ioinfo[irq]->senseid.ciw[ciw_cnt].count,
				                   GFP_KERNEL);
			}
			else
			{
				rcd_buf = alloc_bootmem( ioinfo[irq]->senseid.ciw[ciw_cnt].count);

   		} /* endif */

			if ( rcd_buf == NULL )
			{
				ret = -ENOMEM;	

			} /* endif */

			if ( !ret )
			{
				memset( rcd_buf,
				        '\0',
				        ioinfo[irq]->senseid.ciw[ciw_cnt].count);
       	
				do
				{
					rcd_ccw->cmd_code = ioinfo[irq]->senseid.ciw[ciw_cnt].cmd;
					rcd_ccw->cda      = (__u32)virt_to_phys( rcd_buf );
					rcd_ccw->count    = ioinfo[irq]->senseid.ciw[ciw_cnt].count;
					rcd_ccw->flags    = CCW_FLAG_SLI;

					memset( pdevstat, '\0', sizeof( devstat_t));

					if ( lpm )
					{
						ioflags = DOIO_WAIT_FOR_INTERRUPT
						          | DOIO_VALID_LPM    					
						          | DOIO_DONT_CALL_INTHDLR;
					}
					else
					{
						ioflags =   DOIO_WAIT_FOR_INTERRUPT
						          | DOIO_DONT_CALL_INTHDLR;
						             					
					} /* endif */

					ret = s390_start_IO( irq,
					                     rcd_ccw,
					                     0x00524344,  // == RCD
					                     lpm,
					                     ioflags );

	            switch ( ret ) {
   	         case 0    :
      	      case -EIO :

	               if ( !(pdevstat->flag & (   DEVSTAT_STATUS_PENDING
   	                                      | DEVSTAT_NOT_OPER
      	                                   | DEVSTAT_FLAG_SENSE_AVAIL ) ) )
	               {
   	               retry = 0;  // we got it ...
      	         }
         	      else
            	   {
               	   retry--;    // try again ...

	               } /* endif */

   	            break;

	            default :   // -EBUSY, -ENODEV, ???
   	            retry = 0;

      	      } /* endswitch */

				} while ( retry );

			} /* endif */

			__restore_flags( flags );

		} /* endif */

		/*
		 * on success we update the user input parms
		 */
		if ( ret == 0 )
		{
			*length = ioinfo[irq]->senseid.ciw[ciw_cnt].count;
			*buffer = rcd_buf;
		}
		else
		{
			if ( rcd_buf != NULL )
			{
				if ( init_IRQ_complete )
				{
					kfree( rcd_buf );
				}
				else
				{
					free_bootmem( (unsigned long)rcd_buf,
					              ioinfo[irq]->senseid.ciw[ciw_cnt].count);

   			} /* endif */

			} /* endif */

			*buffer = NULL;
			*length = 0;
    	
		} /* endif */

		if ( emulated )
			free_irq( irq, pdevstat);
	}
	else
	{
		ret = -EOPNOTSUPP;

	} /* endif */

	return( ret );

}

int get_dev_info( int irq, dev_info_t * pdi)
{
	return( get_dev_info_by_irq( irq, pdi));
}

static int __inline__ get_next_available_irq( ioinfo_t *pi)
{
	int ret_val;

	while ( TRUE )
	{
		if ( pi->ui.flags.oper )
		{
			ret_val = pi->irq;
			break;
		}
		else
		{
			pi = pi->next;

			//
			// leave at end of list unconditionally
			//
			if ( pi == NULL )
			{
				ret_val = -ENODEV;
				break;
			}

		} /* endif */

	} /* endwhile */

	return ret_val;
}


int get_irq_first( void )
{
   int ret_irq;

	if ( ioinfo_head )
	{
		if ( ioinfo_head->ui.flags.oper )
		{
			ret_irq = ioinfo_head->irq;
		}
		else if ( ioinfo_head->next )
		{
			ret_irq = get_next_available_irq( ioinfo_head->next );

		}
		else
		{
			ret_irq = -ENODEV;
   	
		} /* endif */
	}
	else
	{
		ret_irq = -ENODEV;

	} /* endif */

	return ret_irq;
}

int get_irq_next( int irq )
{
	int ret_irq;	

	if ( ioinfo[irq] != INVALID_STORAGE_AREA )
	{
		if ( ioinfo[irq]->next )
		{
			if ( ioinfo[irq]->next->ui.flags.oper )
			{
				ret_irq = ioinfo[irq]->next->irq;
			}
			else
			{
				ret_irq = get_next_available_irq( ioinfo[irq]->next );

			} /* endif */
		}
		else
		{
			ret_irq = -ENODEV;     	

		} /* endif */
	}
	else
	{
		ret_irq = -EINVAL;

	} /* endif */

	return ret_irq;
}

int get_dev_info_by_irq( int irq, dev_info_t *pdi)
{

	if ( irq > highest_subchannel || irq < 0 )
	{
		return -ENODEV;
	}
	else if ( pdi == NULL )
	{
		return -EINVAL;
	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);
	}
	else
	{
		pdi->devno = ioinfo[irq]->schib.pmcw.dev;
		pdi->irq   = irq;

		if (     ioinfo[irq]->ui.flags.oper
           && !ioinfo[irq]->ui.flags.unknown )
		{
			pdi->status = 0;
			memcpy( &(pdi->sid_data),
			        &ioinfo[irq]->senseid,
			        sizeof( senseid_t));
		}
		else if ( ioinfo[irq]->ui.flags.unknown )
		{
			pdi->status = DEVSTAT_UNKNOWN_DEV;
			memset( &(pdi->sid_data),
			        '\0',
			        sizeof( senseid_t));
			pdi->sid_data.cu_type = 0xFFFF;

		}
		else
		{
			pdi->status = DEVSTAT_NOT_OPER;
			memset( &(pdi->sid_data),
			        '\0',
			        sizeof( senseid_t));
			pdi->sid_data.cu_type = 0xFFFF;

		} /* endif */

		if ( ioinfo[irq]->ui.flags.ready )
			pdi->status |= DEVSTAT_DEVICE_OWNED;

	} /* endif */

	return 0;
}


int get_dev_info_by_devno( unsigned int devno, dev_info_t *pdi)
{
	int i;
	int rc = -ENODEV;

	if ( devno > 0x0000ffff )
	{
		return -ENODEV;
	}
	else if ( pdi == NULL )
	{
		return -EINVAL;
	}
	else
	{

		for ( i=0; i <= highest_subchannel; i++ )
		{

			if (    ioinfo[i] != INVALID_STORAGE_AREA
			     && ioinfo[i]->schib.pmcw.dev == devno )
			{

				pdi->irq   = i;
				pdi->devno = devno;

				if (    ioinfo[i]->ui.flags.oper
                 && !ioinfo[i]->ui.flags.unknown )
				{
					pdi->status = 0;

					memcpy( &(pdi->sid_data),
					        &ioinfo[i]->senseid,
					        sizeof( senseid_t));
				}
				else if ( ioinfo[i]->ui.flags.unknown )
				{
					pdi->status = DEVSTAT_UNKNOWN_DEV;

					memset( &(pdi->sid_data),
                       '\0',
                       sizeof( senseid_t));

					pdi->sid_data.cu_type = 0xFFFF;
				}
				else
				{
					pdi->status = DEVSTAT_NOT_OPER;
	
					memset( &(pdi->sid_data),
                       '\0',
                       sizeof( senseid_t));

					pdi->sid_data.cu_type = 0xFFFF;

				} /* endif */

				if ( ioinfo[i]->ui.flags.ready )
					pdi->status |= DEVSTAT_DEVICE_OWNED;

				rc = 0; /* found */
				break;

			} /* endif */

		} /* endfor */

		return( rc);

	} /* endif */

}

int get_irq_by_devno( unsigned int devno )
{
	int i;
	int rc = -1;

	if ( devno <= 0x0000ffff )
	{
		for ( i=0; i <= highest_subchannel; i++ )
		{
			if (    (ioinfo[i] != INVALID_STORAGE_AREA )
			     && (ioinfo[i]->schib.pmcw.dev == devno)
			     && (ioinfo[i]->schib.pmcw.dnv == 1    ) )
			{
				rc = i;
				break;

			} /* endif */

		} /* endfor */
	
	} /* endif */

	return( rc);
}

unsigned int get_devno_by_irq( int irq )
{

	if (    ( irq > highest_subchannel            )
	     || ( irq < 0                             )
	     || ( ioinfo[irq] == INVALID_STORAGE_AREA ) )
	{
		return -1;
	
	} /* endif */

	/*
	 * we don't need to check for the device be operational
	 *  as the initial STSCH will always present the device
	 *  number defined by the IOCDS regardless of the device
	 *  existing or not. However, there could be subchannels
	 *  defined who's device number isn't valid ...
	 */
	if ( ioinfo[irq]->schib.pmcw.dnv )
		return( ioinfo[irq]->schib.pmcw.dev );
	else
		return -1;
}

/*
 * s390_device_recognition_irq
 *
 * Used for individual device recognition. Issues the device
 *  independant SenseID command to obtain info the device type.
 *
 */
void s390_device_recognition_irq( int irq )
{
	int           ret;
	unsigned long psw_flags;

	/*
	 * We issue the SenseID command on I/O subchannels we think are
	 *  operational only.
	 */
	if (    ( ioinfo[irq] != INVALID_STORAGE_AREA )	
	     && ( ioinfo[irq]->schib.pmcw.st == 0     )
	     && ( ioinfo[irq]->ui.flags.oper == 1     ) )
	{
		int       irq_ret;
		devstat_t devstat;

	   irq_ret = request_irq( irq,
		                       init_IRQ_handler,
		                       0,
		                       "INIT",
		                       &devstat);

		if ( !irq_ret )
		{
			/*
			 * avoid sync processing (STSCH/MSCH) for every
			 *  single I/O during boot (IPL) processing.
			 */
			spin_lock_irqsave( &sync_isc, psw_flags);
 	
			ret = enable_cpu_sync_isc( irq);

			if ( ret )
			{
				spin_unlock_irqrestore( &sync_isc, psw_flags);
			}
			else
			{
				ioinfo[irq]->ui.flags.syncio  = 1; // global
				ioinfo[irq]->ui.flags.unknown = 0;

				memset( &ioinfo[irq]->senseid, '\0', sizeof( senseid_t));

				s390_SenseID( irq, &ioinfo[irq]->senseid, 0xff );
#if 0	/* FIXME */
				/*
				 * We initially check the configuration data for
				 *  those devices with more than a single path
				 */
				if ( ioinfo[irq]->schib.pmcw.pim != 0x80 )
				{
					char     *prcd;
					int       lrcd;

					ret = read_conf_data( irq, (void **)&prcd, &lrcd, 0 );

					if ( !ret )	// on success only ...
					{
#ifdef CONFIG_DEBUG_IO
						char buffer[80];

						sprintf( buffer,
						         "RCD for device(%04X)/"
						         "subchannel(%04X) returns :\n",
						         ioinfo[irq]->schib.pmcw.dev,
						         irq );

						s390_displayhex( buffer, prcd, lrcd );
#endif      				
						if ( init_IRQ_complete )
						{
							kfree( prcd );
						}
						else
						{
							free_bootmem( (unsigned long)prcd, lrcd );

 			  			} /* endif */

	   			} /* endif */

     	      } /* endif */
#endif
				s390_DevicePathVerification( irq, 0 );

				disable_cpu_sync_isc( irq );

				ioinfo[irq]->ui.flags.syncio = 0; // global

				spin_unlock_irqrestore( &sync_isc, psw_flags);

			} /* endif */  	

			free_irq( irq, &devstat );

		} /* endif */
		
	} /* endif */

}

/*
 * s390_device_recognition_all
 *
 * Used for system wide device recognition.
 *
 */
void s390_device_recognition_all( void)
{
	int irq = 0; /* let's start with subchannel 0 ... */

	do
	{
		s390_device_recognition_irq( irq );

		irq ++;

	} while ( irq <= highest_subchannel );

}


/*
 * s390_search_devices
 *
 * Determines all subchannels available to the system.
 *
 */
void s390_process_subchannels( void)
{
	int   ret;
	int   irq = 0;   /* Evaluate all subchannels starting with 0 ... */

	do
	{
		ret = s390_validate_subchannel( irq, 0);

		if ( ret != -ENXIO)
			irq++;
	
  	} while ( (ret != -ENXIO) && (irq < __MAX_SUBCHANNELS) );

	highest_subchannel = (--irq);

	printk( "Highest subchannel number detected (hex) : %04X\n",
	        highest_subchannel);
}

/*
 * s390_validate_subchannel()
 *
 * Process the subchannel for the requested irq. Returns 1 for valid
 *  subchannels, otherwise 0.
 */
int s390_validate_subchannel( int irq, int enable )
{

	int      retry;     /* retry count for status pending conditions */
	int      ccode;     /* condition code for stsch() only */
	int      ccode2;    /* condition code for other I/O routines */
	schib_t *p_schib;
	int      ret;

	/*
	 * The first subchannel that is not-operational (ccode==3)
	 *  indicates that there aren't any more devices available.
	 */
	if (    ( init_IRQ_complete                   )
	     && ( ioinfo[irq] != INVALID_STORAGE_AREA ) )
	{
		p_schib = &ioinfo[irq]->schib;
	}
	else
	{
		p_schib = &init_schib;

	} /* endif */

	/*
	 * If we knew the device before we assume the worst case ... 	
	 */
	if ( ioinfo[irq] != INVALID_STORAGE_AREA )
	{
		ioinfo[irq]->ui.flags.oper = 0;
		ioinfo[irq]->ui.flags.dval = 0;

	} /* endif */

	ccode = stsch( irq, p_schib);

	if ( !ccode )
	{
		/*
		 * ... just being curious we check for non I/O subchannels
		 */
		if ( p_schib->pmcw.st )
		{
			printk( "Subchannel %04X reports "
			        "non-I/O subchannel type %04X\n",
			        irq,
			        p_schib->pmcw.st);

			if ( ioinfo[irq] != INVALID_STORAGE_AREA )
				ioinfo[irq]->ui.flags.oper = 0;

		} /* endif */

		if ( p_schib->pmcw.dnv )
		{
			if ( ioinfo[irq] == INVALID_STORAGE_AREA )
			{	
				if ( !init_IRQ_complete )
				{
					ioinfo[irq] =
					   (ioinfo_t *)alloc_bootmem( sizeof(ioinfo_t));
				}
				else
				{
					ioinfo[irq] =
					   (ioinfo_t *)kmalloc( sizeof(ioinfo_t),
				                           GFP_KERNEL );

				} /* endif */

				memset( ioinfo[irq], '\0', sizeof( ioinfo_t));
				memcpy( &ioinfo[irq]->schib,
			           &init_schib,
			           sizeof( schib_t));
				ioinfo[irq]->irq_desc.status  = IRQ_DISABLED;
				ioinfo[irq]->irq_desc.handler = &no_irq_type;
			
				/*
				 * We have to insert the new ioinfo element
				 *  into the linked list, either at its head,
				 *  its tail or insert it.
				 */
				if ( ioinfo_head == NULL )  /* first element */
				{
					ioinfo_head = ioinfo[irq];
					ioinfo_tail = ioinfo[irq];
				}
				else if ( irq < ioinfo_head->irq ) /* new head */
				{
					ioinfo[irq]->next = ioinfo_head;
					ioinfo_head->prev = ioinfo[irq];
					ioinfo_head       = ioinfo[irq];
				}
				else if ( irq > ioinfo_tail->irq ) /* new tail */
				{
					ioinfo_tail->next = ioinfo[irq];
					ioinfo[irq]->prev = ioinfo_tail;
					ioinfo_tail       = ioinfo[irq];
				}
				else /* insert element */
				{
					ioinfo_t *pi = ioinfo_head;

					do
					{
						if ( irq < pi->next->irq )
						{
							ioinfo[irq]->next = pi->next;
							ioinfo[irq]->prev = pi;
							pi->next->prev    = ioinfo[irq];
							pi->next          = ioinfo[irq];
							break;
						
						} /* endif */

						pi = pi->next;

					} while ( 1 );
      	
				} /* endif */

			} /* endif */

			// initialize some values ...	
			ioinfo[irq]->ui.flags.pgid_supp = 1;

			ioinfo[irq]->opm =   ioinfo[irq]->schib.pmcw.pim
			                   & ioinfo[irq]->schib.pmcw.pam
			                   & ioinfo[irq]->schib.pmcw.pom;

			printk( "Detected device %04X on subchannel %04X"
			        " - PIM = %02X, PAM = %02X, POM = %02X\n",
			        ioinfo[irq]->schib.pmcw.dev,
			        irq,
			        ioinfo[irq]->schib.pmcw.pim,
			        ioinfo[irq]->schib.pmcw.pam,
			        ioinfo[irq]->schib.pmcw.pom);

			/*
			 * initialize ioinfo structure
			 */
			ioinfo[irq]->irq             = irq;
			ioinfo[irq]->nopfunc         = NULL;
			ioinfo[irq]->ui.flags.busy   = 0;
			ioinfo[irq]->ui.flags.ready  = 0;
			ioinfo[irq]->ui.flags.dval   = 1;
			ioinfo[irq]->devstat.intparm = 0;
			ioinfo[irq]->devstat.devno   = ioinfo[irq]->schib.pmcw.dev;
			ioinfo[irq]->devno           = ioinfo[irq]->schib.pmcw.dev;

			/*
			 * We should have at least one CHPID ...
			 */
			if ( ioinfo[irq]->opm )
			{
				/*
				 * We now have to initially ...
				 *  ... set "interruption sublass"
				 *  ... enable "concurrent sense"
				 *  ... enable "multipath mode" if more than one
				 *        CHPID is available. This is done regardless
				 *        whether multiple paths are available for us.
				 *
				 * Note : we don't enable the device here, this is temporarily
				 *        done during device sensing below.
				 */
				ioinfo[irq]->schib.pmcw.isc     = 3; /* could be smth. else */
				ioinfo[irq]->schib.pmcw.csense  = 1; /* concurrent sense */
				ioinfo[irq]->schib.pmcw.ena     = enable;
				ioinfo[irq]->schib.pmcw.intparm =
				                     ioinfo[irq]->schib.pmcw.dev;

				if (    ( ioinfo[irq]->opm != 0x80 )
				     && ( ioinfo[irq]->opm != 0x40 )
				     && ( ioinfo[irq]->opm != 0x20 )
				     && ( ioinfo[irq]->opm != 0x10 )
				     && ( ioinfo[irq]->opm != 0x08 )
				     && ( ioinfo[irq]->opm != 0x04 )
				     && ( ioinfo[irq]->opm != 0x02 )
				     && ( ioinfo[irq]->opm != 0x01 ) )
				{
					ioinfo[irq]->schib.pmcw.mp = 1; /* multipath mode */

				} /* endif */

				retry = 5;

				do
				{
					ccode2 = msch_err( irq, &ioinfo[irq]->schib);

					switch (ccode2) {
					case 0:  // successful completion
						//
						// concurrent sense facility available ...
						//
						ioinfo[irq]->ui.flags.oper   = 1;
						ioinfo[irq]->ui.flags.consns = 1;
						ret                          = 0;
						break;
      	
					case 1:  // status pending
						//
						// How can we have a pending status as
						//  device is disabled for interrupts ?
						//  Anyway, process it ...
						//
						ioinfo[irq]->ui.flags.s_pend = 1;
						s390_process_IRQ( irq);
						ioinfo[irq]->ui.flags.s_pend = 0;
						retry--;
						ret = -EIO;
						break;
   	
					case 2:  // busy
						/*
						 * we mark it not-oper as we can't
						 *  properly operate it !
						 */
						ioinfo[irq]->ui.flags.oper = 0;
						tod_wait( 100);	/* allow for recovery */
						retry--;
						ret = -EBUSY;
						break;

					case 3:  // not operational
						ioinfo[irq]->ui.flags.oper = 0;
						retry                      = 0;
						ret = -ENODEV;
						break;

					default:
#define PGMCHK_OPERAND_EXC      0x15

						if ( (ccode2 & PGMCHK_OPERAND_EXC) == PGMCHK_OPERAND_EXC )
						{
							/*
							 * re-issue the modify subchannel without trying to
							 *  enable the concurrent sense facility
							 */
							ioinfo[irq]->schib.pmcw.csense = 0;
   	
							ccode2 = msch_err( irq, &ioinfo[irq]->schib);

							if ( ccode2 != 0 )
							{
								printk( " ... msch() (2) failed with CC = %X\n",
								        ccode2 );
								ioinfo[irq]->ui.flags.oper = 0;
								ret                        = -EIO;
							}
							else
							{
								ioinfo[irq]->ui.flags.oper   = 1;
								ioinfo[irq]->ui.flags.consns = 0;
								ret                          = 0;

							} /* endif */
						}
						else
						{
							printk( " ... msch() (1) failed with CC = %X\n",
							        ccode2);
							ioinfo[irq]->ui.flags.oper = 0;
							ret                        = -EIO;

						} /* endif */
   	
						retry  = 0;
						break;

					} /* endswitch */

				} while ( ccode2 && retry );

				if ( (ccode2 != 0) && (ccode2 != 3) && (!retry) )
				{
					printk( " ... msch() retry count for "
					        "subchannel %04X exceeded, CC = %d\n",
					        irq,
					        ccode2);

				} /* endif */
			}
			else
			{
				/* no path available ... */
				ioinfo[irq]->ui.flags.oper = 0;
				ret                        = -ENODEV;    	

			} /* endif */
		}
		else
		{
			ret = -ENODEV;

		} /* endif */
	}
	else
	{
		ret = -ENXIO;

	} /* endif */

	return( ret );
}

/*
 * s390_SenseID
 *
 * Try to obtain the 'control unit'/'device type' information
 *  associated with the subchannel.
 *
 * The function is primarily meant to be called without irq
 *  action handler in place. However, it also allows for
 *  use with an action handler in place. If there is already
 *  an action handler registered assure it can handle the
 *  s390_SenseID() related device interrupts - interruption
 *  parameter used is 0x00E2C9C4 ( SID ).
 */
int s390_SenseID( int irq, senseid_t *sid, __u8 lpm )
{
	ccw1_t     sense_ccw[2];  /* ccw area for SenseID command */
	senseid_t  isid;          /* internal sid */				
	devstat_t  devstat;       /* required by request_irq() */
	__u8       pathmask;      /* calulate path mask */
	__u8       domask;        /* path mask to use */
	int        inlreq;        /* inline request_irq() */
	int        irq_ret;       /* return code */
	devstat_t *pdevstat;      /* ptr to devstat in use */
	int        retry;         /* retry count */
	int        io_retry;      /* retry indicator */

	senseid_t *psid     = sid;/* start with the external buffer */	
	int        sbuffer  = 0; /* switch SID data buffer */

	if ( (irq > highest_subchannel) || (irq < 0 ) )
	{
		return( -ENODEV );

	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);

	} /* endif */

	if ( ioinfo[irq]->ui.flags.oper == 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( !ioinfo[irq]->ui.flags.ready )
	{

		pdevstat = &devstat;

		/*
		 * Perform SENSE ID command processing. We have to request device
		 *  ownership and provide a dummy I/O handler. We issue sync. I/O
		 *  requests and evaluate the devstat area on return therefore
		 *  we don't need a real I/O handler in place.
		 */
		irq_ret = request_irq( irq, init_IRQ_handler, 0, "SID", &devstat);

		if ( irq_ret == 0 )
			inlreq = 1;
	}
	else
	{
		inlreq   = 0;
		irq_ret  = 0;
		pdevstat = ioinfo[irq]->irq_desc.action->dev_id;

  	} /* endif */

	if ( irq_ret == 0 )
   {
      int i;

		s390irq_spin_lock( irq);

		// more than one path installed ?
		if ( ioinfo[irq]->schib.pmcw.pim != 0x80 )
		{
			sense_ccw[0].cmd_code = CCW_CMD_SUSPEND_RECONN;
			sense_ccw[0].cda      = 0;
			sense_ccw[0].count    = 0;
			sense_ccw[0].flags    = CCW_FLAG_SLI | CCW_FLAG_CC;

			sense_ccw[1].cmd_code = CCW_CMD_SENSE_ID;
			sense_ccw[1].cda      = (__u32)virt_to_phys( sid );
			sense_ccw[1].count    = sizeof( senseid_t);
			sense_ccw[1].flags    = CCW_FLAG_SLI;
		}
		else
		{
			sense_ccw[0].cmd_code = CCW_CMD_SENSE_ID;
			sense_ccw[0].cda      = (__u32)virt_to_phys( sid );
			sense_ccw[0].count    = sizeof( senseid_t);
			sense_ccw[0].flags    = CCW_FLAG_SLI;

		} /* endif */

		for ( i = 0 ; (i < 8) ; i++ )
		{
			pathmask = 0x80 >> i;						

			domask = ioinfo[irq]->opm & pathmask;

			if ( lpm )
				domask &= lpm;

			if ( domask )
			{
				psid->cu_type    = 0xFFFF;  /* initialize fields ... */
				psid->cu_model   = 0;
				psid->dev_type   = 0;
				psid->dev_model  = 0;

				retry            = 5;  /* retry count    */
				io_retry         = 1;  /* enable retries */

				/*
				 * We now issue a SenseID request. In case of BUSY,
				 *  STATUS PENDING or non-CMD_REJECT error conditions
				 *  we run simple retries.
				 */
				do
				{
					memset( pdevstat, '\0', sizeof( devstat_t) );

					irq_ret = s390_start_IO( irq,
					                         sense_ccw,
					                         0x00E2C9C4,  // == SID
			   		                      domask,
					                         DOIO_WAIT_FOR_INTERRUPT
					                          | DOIO_TIMEOUT
					                          | DOIO_VALID_LPM
					                          | DOIO_DONT_CALL_INTHDLR );

					//
					// The OSA_E FE card possibly causes -ETIMEDOUT
					//  conditions, as the SenseID may stay start
					//  pending. This will cause start_IO() to finally
					//  halt the operation we should retry. If the halt
					//  fails this may cause -EBUSY we simply retry
					//  and eventually clean up with free_irq().   	     		
					//

					if ( psid->cu_type  == 0xFFFF )
					{
						if ( pdevstat->flag & DEVSTAT_STATUS_PENDING )
						{
#ifdef CONFIG_DEBUG_IO
							printk( "SenseID : device %04X on "
							        "Subchannel %04X "
							        "reports pending status, "
							        "retry : %d\n",
							        ioinfo[irq]->schib.pmcw.dev,
					   		     irq,
							        retry);
#endif
						} /* endif */

						if ( pdevstat->flag & DEVSTAT_FLAG_SENSE_AVAIL )
						{
							/*
							 * if the device doesn't support the SenseID
							 *  command further retries wouldn't help ...
							 */
							if (  pdevstat->ii.sense.data[0]
							    & (SNS0_CMD_REJECT | SNS0_INTERVENTION_REQ) )
							{
#ifdef CONFIG_DEBUG_IO
								printk( "SenseID : device %04X on "
								        "Subchannel %04X "
								        "reports cmd reject or "
								        "intervention required\n",
								        ioinfo[irq]->schib.pmcw.dev,
								        irq);
#endif
								io_retry = 1;
							}
#ifdef CONFIG_DEBUG_IO
							else
							{
								printk( "SenseID : UC on "
								        "dev %04X, "
								        "retry %d, "
								        "lpum %02X, "
								        "cnt %02d, "
								        "sns :"
								        " %02X%02X%02X%02X "
								        "%02X%02X%02X%02X ...\n",
								        ioinfo[irq]->schib.pmcw.dev,
								        retry,
								        pdevstat->lpum,
								        pdevstat->scnt,
								        pdevstat->ii.sense.data[0],
								        pdevstat->ii.sense.data[1],
								        pdevstat->ii.sense.data[2],
								        pdevstat->ii.sense.data[3],
								        pdevstat->ii.sense.data[4],
								        pdevstat->ii.sense.data[5],
								        pdevstat->ii.sense.data[6],
								        pdevstat->ii.sense.data[7]);

							} /* endif */
#endif
						}
						else if (    ( pdevstat->flag & DEVSTAT_NOT_OPER )
					             || ( irq_ret        == -ENODEV         ) )
						{
#ifdef CONFIG_DEBUG_IO
							printk( "SenseID : path %02X for "
							        "device %04X on "
							        "subchannel %04X "
							        "is 'not operational'\n",
							        domask,
							        ioinfo[irq]->schib.pmcw.dev,
							        irq);
#endif

							io_retry          = 0;
							ioinfo[irq]->opm &= ~domask;
      	
						}
#ifdef CONFIG_DEBUG_IO
						else if (     (pdevstat->flag !=
						                    (   DEVSTAT_START_FUNCTION
						                      | DEVSTAT_FINAL_STATUS    ) )
						           && !(pdevstat->flag &
						                    DEVSTAT_STATUS_PENDING        ) )
						{
							printk( "SenseID : start_IO() for "
							        "device %04X on "
							        "subchannel %04X "
							        "returns %d, retry %d, "
							        "status %04X\n",
							        ioinfo[irq]->schib.pmcw.dev,
							        irq,
							        irq_ret,
							        retry,
							        pdevstat->flag);

						} /* endif */
#endif
					}
					else   // we got it ...
					{
						if ( !sbuffer )	// switch buffers
						{
							/*
							 * we report back the
							 *  first hit only
							 */
							psid = &isid;

							if ( ioinfo[irq]->schib.pmcw.pim != 0x80 )
							{
								sense_ccw[1].cda = (__u32)virt_to_phys( psid );
							}
							else
							{
								sense_ccw[0].cda = (__u32)virt_to_phys( psid );

							} /* endif */

							/*
							 * if just the very first
							 *  was requested to be
							 *  sensed disable further
							 *  scans.
							 */	
							if ( !lpm )
								lpm = domask;

							sbuffer = 1;

						} /* endif */

					   if ( pdevstat->rescnt < (sizeof( senseid_t) - 8) )
						{
							ioinfo[irq]->ui.flags.esid = 1;
       							
						} /* endif */

						io_retry = 0;
						
					} /* endif */

					if ( io_retry )
					{
						retry--;

						if ( retry == 0 )
						{
							io_retry = 0;

						} /* endif */
      	
					} /* endif */
	
				} while ( (io_retry) );

 			} /* endif - domask */

		} /* endfor */

		s390irq_spin_unlock( irq);

		/*
		 * If we installed the irq action handler we have to
		 *  release it too.
		 */
		if ( inlreq )
			free_irq( irq, pdevstat);

		/*
		 * if running under VM check there ... perhaps we should do
		 *  only if we suffered a command reject, but it doesn't harm
		 */
		if (    ( sid->cu_type == 0xFFFF    )
		     && ( MACHINE_IS_VM              ) )
		{
			VM_virtual_device_info( ioinfo[irq]->schib.pmcw.dev,
			                        sid );
		} /* endif */

		if ( sid->cu_type == 0xFFFF )
		{
			/*
			 * SenseID CU-type of 0xffff indicates that no device
			 *  information could be retrieved (pre-init value).
			 *
			 * If we can't couldn't identify the device type we
			 *  consider the device "not operational".
			 */
#ifdef CONFIG_DEBUG_IO
			printk( "SenseID : unknown device %04X on subchannel %04X\n",
			        ioinfo[irq]->schib.pmcw.dev,
			        irq);
#endif
			ioinfo[irq]->ui.flags.unknown = 1;

		} /* endif */

		/*
		 * Issue device info message if unit was operational .
		 */
		if ( !ioinfo[irq]->ui.flags.unknown )
		{
			if ( sid->dev_type != 0 )
			{
				printk( "SenseID : device %04X reports: CU  Type/Mod = %04X/%02X,"
				        " Dev Type/Mod = %04X/%02X\n",
				        ioinfo[irq]->schib.pmcw.dev,
				        sid->cu_type,
				        sid->cu_model,
				        sid->dev_type,
				        sid->dev_model);
			}
			else
			{
				printk( "SenseID : device %04X reports:"
				        " Dev Type/Mod = %04X/%02X\n",
				        ioinfo[irq]->schib.pmcw.dev,
				        sid->cu_type,
				        sid->cu_model);

			} /* endif */

		} /* endif */

		if ( !ioinfo[irq]->ui.flags.unknown )
			irq_ret = 0;
		else
			irq_ret = -ENODEV;

	} /* endif */

   return( irq_ret );
}

static int __inline__ s390_SetMultiPath( int irq )
{
	int cc;

	cc = stsch( irq, &ioinfo[irq]->schib );

	if ( !cc )
	{
		ioinfo[irq]->schib.pmcw.mp = 1;     /* multipath mode */

		cc = msch( irq, &ioinfo[irq]->schib );

	} /* endif */

	return( cc);
}

/*
 * Device Path Verification
 *
 * Path verification is accomplished by checking which paths (CHPIDs) are
 *  available. Further, a path group ID is set, if possible in multipath
 *  mode, otherwise in single path mode.
 *
 */
int s390_DevicePathVerification( int irq, __u8 usermask )
{
#if 1
	int  ccode;
	__u8 pathmask;
	__u8 domask;

	int ret = 0;

	if ( ioinfo[irq]->ui.flags.pgid_supp == 0 )
	{
		return( 0);	// just exit ...

	} /* endif */

	ccode = stsch( irq, &(ioinfo[irq]->schib) );

	if ( ccode )
	{
		ret = -ENODEV;
	}
	else if ( ioinfo[irq]->schib.pmcw.pim == 0x80 )
	{
		/*
		 * no error, just not required for single path only devices
		 */	
		ioinfo[irq]->ui.flags.pgid_supp = 0;
		ret = 0;
	}
	else
	{
		int    i;
		pgid_t pgid;
		__u8   dev_path;
		int    first  = 1;

		ioinfo[irq]->opm =   ioinfo[irq]->schib.pmcw.pim
		                   & ioinfo[irq]->schib.pmcw.pam
		                   & ioinfo[irq]->schib.pmcw.pom;

		if ( usermask )
		{
			dev_path = usermask;
		}
		else
		{
			dev_path = ioinfo[irq]->opm;

		} /* endif */

		/*
		 * let's build a path group ID if we don't have one yet
		 */
		if ( ioinfo[irq]->ui.flags.pgid == 0)
		{
			ioinfo[irq]->pgid.cpu_addr  = *(__u16 *)__LC_CPUADDR;
			ioinfo[irq]->pgid.cpu_id    = ((cpuid_t *)__LC_CPUID)->ident;
			ioinfo[irq]->pgid.cpu_model = ((cpuid_t *)__LC_CPUID)->machine;
			ioinfo[irq]->pgid.tod_high  = *(__u32 *)&irq_IPL_TOD;

			ioinfo[irq]->ui.flags.pgid  = 1;

		} /* endif */     		

		memcpy( &pgid, &ioinfo[irq]->pgid, sizeof(pgid_t));

		for ( i = 0; i < 8 && !ret ; i++)
		{
			pathmask = 0x80 >> i;						

			domask = dev_path & pathmask;

			if ( domask )
			{
				ret = s390_SetPGID( irq, domask, &pgid );

				/*
				 * For the *first* path we are prepared
				 *  for recovery
				 *
				 *  - If we fail setting the PGID we assume its
				 *     using  a different PGID already (VM) we
				 *     try to sense.
				 */
				if ( ret == -EOPNOTSUPP && first )
				{
					*(int *)&pgid = 0;
					
					ret   = s390_SensePGID( irq, domask, &pgid);
					first = 0;

					if ( ret == 0 )
					{
						/*
						 * Check whether we retrieved
						 *  a reasonable PGID ...
						 */	
						if ( pgid.inf.ps.state1 == SNID_STATE1_GROUPED )
						{
							memcpy( &(ioinfo[irq]->pgid),
							        &pgid,
							        sizeof(pgid_t) );
						}
						else // ungrouped or garbage ...
						{
							ret = -EOPNOTSUPP;

						} /* endif */
					}
					else
					{
						ioinfo[irq]->ui.flags.pgid_supp = 0;

#ifdef CONFIG_DEBUG_IO
						printk( "PathVerification(%04X) "
						        "- Device %04X doesn't "
						        " support path grouping\n",
						        irq,
						        ioinfo[irq]->schib.pmcw.dev);
#endif

					} /* endif */
				}
				else if ( ret )
				{

#ifdef CONFIG_DEBUG_IO
						printk( "PathVerification(%04X) "
						        "- Device %04X doesn't "
						        " support path grouping\n",
						        irq,
						        ioinfo[irq]->schib.pmcw.dev);

#endif

					ioinfo[irq]->ui.flags.pgid_supp = 0;

				} /* endif */

			} /* endif */

		} /* endfor */

	} /* endif */

	return ret;
#else
	return 0;
#endif
}

/*
 * s390_SetPGID
 *
 * Set Path Group ID
 *
 */
int s390_SetPGID( int irq, __u8 lpm, pgid_t *pgid )
{
	ccw1_t     spid_ccw[2]; /* ccw area for SPID command */
	devstat_t  devstat;     /* required by request_irq() */
	devstat_t *pdevstat = &devstat;

	int        irq_ret = 0; /* return code */
	int        retry   = 5; /* retry count */
	int        inlreq  = 0; /* inline request_irq() */
	int        mpath   = 1; /* try multi-path first */

	if ( (irq > highest_subchannel) || (irq < 0 ) )
	{
		return( -ENODEV );

	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);

	} /* endif */

	if ( ioinfo[irq]->ui.flags.oper == 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( !ioinfo[irq]->ui.flags.ready )
	{
		/*
		 * Perform SENSE ID command processing. We have to request device
		 *  ownership and provide a dummy I/O handler. We issue sync. I/O
		 *  requests and evaluate the devstat area on return therefore
		 *  we don't need a real I/O handler in place.
		 */
		irq_ret = request_irq( irq,
		                       init_IRQ_handler,
		                       0,
		                       "SPID",
		                       pdevstat);

		if ( irq_ret == 0 )
			inlreq = 1;
   }
	else
	{
		pdevstat = ioinfo[irq]->irq_desc.action->dev_id;

	} /* endif */

	if ( irq_ret == 0 )
	{
		s390irq_spin_lock( irq);

		spid_ccw[0].cmd_code = 0x5B;	/* suspend multipath reconnect */
		spid_ccw[0].cda      = 0;
		spid_ccw[0].count    = 0;
		spid_ccw[0].flags    = CCW_FLAG_SLI | CCW_FLAG_CC;

		spid_ccw[1].cmd_code = CCW_CMD_SET_PGID;
		spid_ccw[1].cda      = (__u32)virt_to_phys( pgid );
		spid_ccw[1].count    = sizeof( pgid_t);
		spid_ccw[1].flags    = CCW_FLAG_SLI;

		pgid->inf.fc = SPID_FUNC_MULTI_PATH | SPID_FUNC_ESTABLISH;

		/*
		 * We now issue a SenseID request. In case of BUSY
		 *  or STATUS PENDING conditions we retry 5 times.
		 */
		do
		{
			memset( pdevstat, '\0', sizeof( devstat_t) );

			irq_ret = s390_start_IO( irq,
			                         spid_ccw,
			                         0xE2D7C9C4,  // == SPID
			                         lpm,         // n/a
			                         DOIO_WAIT_FOR_INTERRUPT
			                          | DOIO_VALID_LPM
			                          | DOIO_DONT_CALL_INTHDLR );

			if ( !irq_ret )
			{
				if ( pdevstat->flag & DEVSTAT_STATUS_PENDING )
				{
#ifdef CONFIG_DEBUG_IO
					printk( "SPID - Device %04X "
					        "on Subchannel %04X "
					        "reports pending status, "
					        "retry : %d\n",
					        ioinfo[irq]->schib.pmcw.dev,
					        irq,
					        retry);
#endif
				} /* endif */

				if ( pdevstat->flag == (   DEVSTAT_START_FUNCTION
				                         | DEVSTAT_FINAL_STATUS   ) )
				{
					retry = 0;	// successfully set ...
				}
				else if ( pdevstat->flag & DEVSTAT_FLAG_SENSE_AVAIL )
				{
					/*
					 * If the device doesn't support the
					 *  Sense Path Group ID command
					 *  further retries wouldn't help ...
					 */
					if ( pdevstat->ii.sense.data[0] & SNS0_CMD_REJECT )
					{
						if ( mpath )
						{
							pgid->inf.fc =   SPID_FUNC_SINGLE_PATH
							               | SPID_FUNC_ESTABLISH;
							mpath        = 0;
							retry--;
						}
						else
						{
							irq_ret = -EOPNOTSUPP;
							retry   = 0;			

						} /* endif */
					}
#ifdef CONFIG_DEBUG_IO
					else
					{
						printk( "SPID - device %04X,"
						        " unit check,"
						        " retry %d, cnt %02d,"
						        " sns :"
						        " %02X%02X%02X%02X %02X%02X%02X%02X ...\n",
						        ioinfo[irq]->schib.pmcw.dev,
						        retry,
						        pdevstat->scnt,
						        pdevstat->ii.sense.data[0],
						        pdevstat->ii.sense.data[1],
						        pdevstat->ii.sense.data[2],
						        pdevstat->ii.sense.data[3],
						        pdevstat->ii.sense.data[4],
						        pdevstat->ii.sense.data[5],
						        pdevstat->ii.sense.data[6],
						        pdevstat->ii.sense.data[7]);

					} /* endif */
#endif
				}
				else if ( pdevstat->flag & DEVSTAT_NOT_OPER )
				{
					printk( "SPID - Device %04X "
					        "on Subchannel %04X "
					        "became 'not operational'\n",
					        ioinfo[irq]->schib.pmcw.dev,
					        irq);

					retry = 0;

				} /* endif */
			}
			else if ( irq_ret != -ENODEV )
			{
				retry--;
			}
			else
			{
				retry = 0;

			} /* endif */

		} while ( retry > 0 );

		s390irq_spin_unlock( irq);

		/*
		 * If we installed the irq action handler we have to
		 *  release it too.
		 */
		if ( inlreq )
			free_irq( irq, pdevstat);

	} /* endif */

   return( irq_ret );
}


/*
 * s390_SensePGID
 *
 * Sense Path Group ID
 *
 */
int s390_SensePGID( int irq, __u8 lpm, pgid_t *pgid )
{
	ccw1_t     snid_ccw;    /* ccw area for SNID command */
	devstat_t  devstat;     /* required by request_irq() */
	devstat_t *pdevstat = &devstat;

	int        irq_ret = 0; /* return code */
	int        retry   = 5; /* retry count */
	int        inlreq  = 0; /* inline request_irq() */

	if ( (irq > highest_subchannel) || (irq < 0 ) )
	{
		return( -ENODEV );

	}
	else if ( ioinfo[irq] == INVALID_STORAGE_AREA )
	{
		return( -ENODEV);

	} /* endif */

	if ( ioinfo[irq]->ui.flags.oper == 0 )
	{
		return( -ENODEV );

	} /* endif */

	if ( !ioinfo[irq]->ui.flags.ready )
	{
		/*
		 * Perform SENSE ID command processing. We have to request device
		 *  ownership and provide a dummy I/O handler. We issue sync. I/O
		 *  requests and evaluate the devstat area on return therefore
		 *  we don't need a real I/O handler in place.
		 */
		irq_ret = request_irq( irq,
		                       init_IRQ_handler,
		                       0,
		                       "SNID",
		                       pdevstat);

		if ( irq_ret == 0 )
			inlreq = 1;

   }
	else
	{
		pdevstat = ioinfo[irq]->irq_desc.action->dev_id;

	} /* endif */

	if ( irq_ret == 0 )
	{
		s390irq_spin_lock( irq);

		snid_ccw.cmd_code = CCW_CMD_SENSE_PGID;
		snid_ccw.cda      = (__u32)virt_to_phys( pgid );
		snid_ccw.count    = sizeof( pgid_t);
		snid_ccw.flags    = CCW_FLAG_SLI;

		/*
		 * We now issue a SenseID request. In case of BUSY
		 *  or STATUS PENDING conditions we retry 5 times.
		 */
		do
		{
			memset( pdevstat, '\0', sizeof( devstat_t) );

			irq_ret = s390_start_IO( irq,
			                         &snid_ccw,
			                         0xE2D5C9C4,  // == SNID
			                         lpm,         // n/a
			                         DOIO_WAIT_FOR_INTERRUPT
			                          | DOIO_VALID_LPM
			                          | DOIO_DONT_CALL_INTHDLR );

			if ( irq_ret == 0 )
			{
				if ( pdevstat->flag & DEVSTAT_FLAG_SENSE_AVAIL )
				{
					/*
					 * If the device doesn't support the
					 *  Sense Path Group ID command
					 *  further retries wouldn't help ...
					 */
					if ( pdevstat->ii.sense.data[0] & SNS0_CMD_REJECT )
					{
						retry   = 0;
						irq_ret = -EOPNOTSUPP;
					}
					else
					{
#ifdef CONFIG_DEBUG_IO
						printk( "SNID - device %04X,"
						        " unit check,"
						        " flag %04X, "
						        " retry %d, cnt %02d,"
						        " sns :"
						        " %02X%02X%02X%02X %02X%02X%02X%02X ...\n",
						        ioinfo[irq]->schib.pmcw.dev,
						        pdevstat->flag,
						        retry,
						        pdevstat->scnt,
						        pdevstat->ii.sense.data[0],
						        pdevstat->ii.sense.data[1],
						        pdevstat->ii.sense.data[2],
						        pdevstat->ii.sense.data[3],
						        pdevstat->ii.sense.data[4],
						        pdevstat->ii.sense.data[5],
						        pdevstat->ii.sense.data[6],
						        pdevstat->ii.sense.data[7]);

#endif
						retry--;

					} /* endif */
				}
				else if ( pdevstat->flag & DEVSTAT_NOT_OPER )
				{
					printk( "SNID - Device %04X "
					        "on Subchannel %04X "
					        "became 'not operational'\n",
					        ioinfo[irq]->schib.pmcw.dev,
					        irq);

					retry = 0;

				}
				else
				{
					retry = 0; // success ...

				} /* endif */
			}
			else if ( irq_ret != -ENODEV ) // -EIO, or -EBUSY
			{
#ifdef CONFIG_DEBUG_IO
				if ( pdevstat->flag & DEVSTAT_STATUS_PENDING )
				{
					printk( "SNID - Device %04X "
					        "on Subchannel %04X "
					        "reports pending status, "
					        "retry : %d\n",
					        ioinfo[irq]->schib.pmcw.dev,
					        irq,
					        retry);
				} /* endif */
#endif

				printk( "SNID - device %04X,"
				        " start_io() reports rc : %d, retrying ...\n",
				        ioinfo[irq]->schib.pmcw.dev,
				        irq_ret);
				retry--;
			}
			else	// -ENODEV ...
			{
				retry = 0;

			} /* endif */

		} while ( retry > 0 );

		s390irq_spin_unlock( irq);

		/*
		 * If we installed the irq action handler we have to
		 *  release it too.
		 */
		if ( inlreq )
			free_irq( irq, pdevstat);

	} /* endif */

   return( irq_ret );
}

/*
 * s390_do_crw_pending
 *
 * Called by the machine check handler to process CRW pending
 *  conditions. It may be a single CRW, or CRWs may be chained.
 *
 * Note : we currently process CRWs for subchannel source only
 */
void s390_do_crw_pending( crwe_t *pcrwe )
{
	int irq;
	int dev_oper = 0;
	int dev_no   = -1;	
	int lock     = 0;

#ifdef CONFIG_DEBUG_CRW
	printk( "do_crw_pending : starting ...\n");
#endif

	while ( pcrwe != NULL )
	{
		switch ( pcrwe->crw.rsc ) {	
		case CRW_RSC_SCH :

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : source is "
			        "subchannel\n");
#endif
			irq = pcrwe->crw.rsid;

			/*
			 * If the device isn't known yet
			 *   we can't lock it ...
			 */
	      if ( ioinfo[irq] != INVALID_STORAGE_AREA )
			{
				s390irq_spin_lock( irq );
            lock = 1;

				dev_oper = ioinfo[irq]->ui.flags.oper;

				if ( ioinfo[irq]->ui.flags.dval )
					dev_no = ioinfo[irq]->devno;

			} /* endif */

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : subchannel validation - start ...\n");
#endif
			s390_validate_subchannel( irq, 0 );

			if ( irq > highest_subchannel )
				highest_subchannel = irq;

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : subchannel validation - done\n");
#endif
			/*
			 * After the validate processing
			 *   the ioinfo control block
			 *   should be allocated ...
			 */
			if ( lock )
			{
				s390irq_spin_unlock( irq );

			} /* endif */

#ifdef CONFIG_DEBUG_CRW
			if ( ioinfo[irq] != INVALID_STORAGE_AREA )
			{
				printk( "do_crw_pending : ioinfo at %08X\n",
				        (unsigned)ioinfo[irq]);

			} /* endif */
#endif

			if ( ioinfo[irq] != INVALID_STORAGE_AREA )
			{
				if ( ioinfo[irq]->ui.flags.oper == 0 )
				{
					/*
					 * If the device has gone
					 *  call not oper handler        	
					 */       	
					if (    (             dev_oper == 1    )
					     && ( ioinfo[irq]->nopfunc != NULL ) )
					{

						free_irq( irq,
                                                          ioinfo[irq]->irq_desc.action->dev_id );
						ioinfo[irq]->nopfunc( irq,
						    DEVSTAT_DEVICE_GONE );				
					} /* endif */
				}
				else
				{
#ifdef CONFIG_DEBUG_CRW
					printk( "do_crw_pending : device "
					        "recognition - start ...\n");
#endif
					s390_device_recognition_irq( irq );

#ifdef CONFIG_DEBUG_CRW
					printk( "do_crw_pending : device "
					        "recognition - done\n");
#endif
		
					/*
					 * the device became operational
					 */
					if ( dev_oper == 0 )
					{
						devreg_t *pdevreg;

						pdevreg = s390_search_devreg( ioinfo[irq] );

						if ( pdevreg != NULL )
						{
							if ( pdevreg->oper_func != NULL )
								pdevreg->oper_func( irq, pdevreg );

						} /* endif */
					}
					/*
					 * ... it is and was operational, but
					 *      the devno may have changed
					 */
					else if ( ioinfo[irq]->devno != dev_no )   					
					{
						ioinfo[irq]->nopfunc( irq,
						                      DEVSTAT_REVALIDATE );				

					} /* endif */

				} /* endif */

			} /* endif */

			break;

		case CRW_RSC_MONITOR :

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : source is "
			        "monitoring facility\n");
#endif
			break;

		case CRW_RSC_CPATH :   	

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : source is "
			        "channel path\n");
#endif
			break;

		case CRW_RSC_CONFIG : 	

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : source is "
			        "configuration-alert facility\n");
#endif
			break;

		case CRW_RSC_CSS :

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : source is "
			        "channel path\n");
#endif
			break;

		default :

#ifdef CONFIG_DEBUG_CRW
			printk( "do_crw_pending : unknown source\n");
#endif
			break;		

		} /* endswitch */

		pcrwe = pcrwe->crwe_next;

	} /* endwhile */

#ifdef CONFIG_DEBUG_CRW
	printk( "do_crw_pending : done\n");
#endif

   return;
}


/* added by Holger Smolinski for reipl support in reipl.S */
extern void do_reipl (int);
void
reipl ( int sch )
{
	int i;

	for ( i = 0; i < highest_subchannel; i ++ ) {
	    free_irq ( i, (void*)REIPL_DEVID_MAGIC );
	}
	do_reipl( 0x10000 | sch );
}

