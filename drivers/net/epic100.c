/* epic100.c: A SMC 83c170 EPIC/100 Fast Ethernet driver for Linux. */
/*
	Written/copyright 1997-1998 by Donald Becker.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.
	All other rights reserved.

	This driver is for the SMC83c170/175 "EPIC" series, as used on the
	SMC EtherPower II 9432 PCI adapter, and several CardBus cards.

	The author may be reached as becker@CESDIS.gsfc.nasa.gov, or C/O
	USRA Center of Excellence in Space Data and Information Sciences
	   Code 930.5, Goddard Space Flight Center, Greenbelt MD 20771

	Information and updates available at
	http://cesdis.gsfc.nasa.gov/linux/drivers/epic100.html



				Theory of Operation

I. Board Compatibility

This device driver is designed for the SMC "EPIC/100", the SMC
single-chip Ethernet controllers for PCI.  This chip is used on
the SMC EtherPower II boards.

II. Board-specific settings

PCI bus devices are configured by the system at boot time, so no jumpers
need to be set on the board.  The system BIOS will assign the
PCI INTA signal to a (preferably otherwise unused) system IRQ line.
Note: Kernel versions earlier than 1.3.73 do not support shared PCI
interrupt lines.

III. Driver operation

IIIa. Ring buffers

IVb. References

http://www.smsc.com/main/datasheets/83c171.pdf
http://www.smsc.com/main/datasheets/83c175.pdf
http://cesdis.gsfc.nasa.gov/linux/misc/NWay.html
http://www.national.com/pf/DP/DP83840A.html

IVc. Errata

*/



static const char *version =
"epic100.c:v1.04 8/23/98 Donald Becker http://cesdis.gsfc.nasa.gov/linux/drivers/epic100.html\n";

/* A few user-configurable values. */

/* Keep the ring sizes a power of two for efficiency.
   Making the Tx ring too large decreases the effectiveness of channel
   bonding and packet priority.
   There are no ill effects from too-large receive rings. */
#define TX_RING_SIZE	16
#define RX_RING_SIZE	32

/* Set the copy breakpoint for the copy-only-tiny-frames scheme.
   Setting to > 1518 effectively disables this feature. */
static int rx_copybreak = 200;

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */
static int max_interrupt_work = 10;

/* Operational parameters that usually are not changed. */
/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT  ((2000*HZ)/1000)

#define PKT_BUF_SZ		1536			/* Size of each temporary Rx buffer.*/

/* Bytes transferred to chip before transmission starts. */
#define TX_FIFO_THRESH 256		/* Rounded down to 4 byte units. */
#define RX_FIFO_THRESH 1		/* 0-3, 0==32, 64,96, or 3==128 bytes  */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/delay.h>

#include <asm/processor.h>		/* Processor type for cache alignment. */
#include <asm/bitops.h>
#include <asm/io.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

/* Kernel compatibility defines, common to David Hind's PCMCIA package.
   This is only in the support-all-kernels source code. */

#define RUN_AT(x) (jiffies + (x))

#define EPIC100_MODULE_NAME "epic100"
#define PFX EPIC100_MODULE_NAME ": "

MODULE_AUTHOR("Donald Becker <becker@cesdis.gsfc.nasa.gov>");
MODULE_DESCRIPTION("SMC 83c170 EPIC series Ethernet driver");
MODULE_PARM(debug, "i");
MODULE_PARM(options, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(full_duplex, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(rx_copybreak, "i");
MODULE_PARM(max_interrupt_work, "i");

/* The I/O extent. */
#define EPIC_TOTAL_SIZE 0x100

#define epic_debug debug
static int epic_debug = 1;

/* The rest of these values should never change. */


enum pci_flags_bit {
	PCI_USES_IO=1, PCI_USES_MEM=2, PCI_USES_MASTER=4,
	PCI_ADDR0=0x10<<0, PCI_ADDR1=0x10<<1, PCI_ADDR2=0x10<<2, PCI_ADDR3=0x10<<3,
};


typedef enum {
	SMSC_83C170,
	SMSC_83C175,
} chip_t;


struct epic100_chip_info {
	const char *name;
};


/* indexed by chip_t */
static struct epic100_chip_info epic100_chip_info[] __devinitdata = {
	{ "SMSC EPIC/100 83c170" },
	{ "SMSC EPIC/C 83c175" },
};


static struct pci_device_id epic100_pci_tbl[] __devinitdata = {
	{ 0x10B8, 0x0005, PCI_ANY_ID, PCI_ANY_ID, 0, 0, SMSC_83C170, },
	{ 0x10B8, 0x0006, PCI_ANY_ID, PCI_ANY_ID, 0, 0, SMSC_83C175, },
	{ 0,},
};
MODULE_DEVICE_TABLE (pci, epic100_pci_tbl);

	
/* Offsets to registers, using the (ugh) SMC names. */
enum epic_registers {
	COMMAND = 0,
	INTSTAT = 4,
	INTMASK = 8,
	GENCTL = 0x0C,
	NVCTL = 0x10,
	EECTL = 0x14,
	PCIBurstCnt = 0x18,
	TEST1 = 0x1C,
	CRCCNT = 0x20,
	ALICNT = 0x24,
	MPCNT = 0x28,		/* Rx error counters. */
	MIICtrl = 0x30,
	MIIData = 0x34,
	MIICfg = 0x38,
	LAN0 = 64,		/* MAC address. */
	MC0 = 80,		/* Multicast filter table. */
	RxCtrl = 96,
	TxCtrl = 112,
	TxSTAT = 0x74,
	PRxCDAR = 0x84,
	RxSTAT = 0xA4,
	EarlyRx = 0xB0,
	PTxCDAR = 0xC4,
	TxThresh = 0xDC,
};


/* Interrupt register bits, using my own meaningful names. */
enum IntrStatus {
	TxIdle = 0x40000,
	RxIdle = 0x20000,
	IntrSummary = 0x010000,
	PCIBusErr170 = 0x7000,
	PCIBusErr175 = 0x1000,
	PhyEvent175 = 0x8000,
	RxStarted = 0x0800,
	RxEarlyWarn = 0x0400,
	CntFull = 0x0200,
	TxUnderrun = 0x0100,
	TxEmpty = 0x0080,
	TxDone = 0x0020, RxError = 0x0010,
	RxOverflow = 0x0008,
	RxFull = 0x0004,
	RxHeader = 0x0002,
	RxDone = 0x0001,
};

/* The EPIC100 Rx and Tx buffer descriptors. */

struct epic_tx_desc {
	s16 status;
	u16 txlength;
	u32 bufaddr;
	u16 buflength;
	u16 control;
	u32 next;
};

struct epic_rx_desc {
	s16 status;
	u16 rxlength;
	u32 bufaddr;
	u32 buflength;
	u32 next;
};

struct epic_private {
	char devname[8];			/* Used only for kernel debugging. */
	const char *product_name;
	struct net_device *next_module;
	
	spinlock_t lock;

	/* Tx and Rx rings here so that they remain paragraph aligned. */
	struct epic_rx_desc rx_ring[RX_RING_SIZE];
	struct epic_tx_desc tx_ring[TX_RING_SIZE];
	/* The saved address of a sent-in-place packet/buffer, for skfree(). */
	struct sk_buff* tx_skbuff[TX_RING_SIZE];
	/* The addresses of receive-in-place skbuffs. */
	struct sk_buff* rx_skbuff[RX_RING_SIZE];

	/* Ring pointers. */
	unsigned int cur_rx, cur_tx;		/* The next free ring entry */
	unsigned int dirty_rx, dirty_tx;	/* The ring entries to be free()ed. */

	struct pci_dev *pdev;
	u16 chip_id;

	struct net_device_stats stats;
	struct timer_list timer;			/* Media selection timer. */
	unsigned char mc_filter[8];
	signed char phys[4];				/* MII device addresses. */
	unsigned int tx_full:1;				/* The Tx queue is full. */
	unsigned int full_duplex:1;			/* Current duplex setting. */
	unsigned int force_fd:1;			/* Full-duplex operation requested. */
	unsigned int default_port:4;		/* Last dev->if_port value. */
	unsigned int media2:4;				/* Secondary monitored media port. */
	unsigned int medialock:1;			/* Don't sense media type. */
	unsigned int mediasense:1;			/* Media sensing in progress. */
	int pad0, pad1;						/* Used for 8-byte alignment */
};

/* Used to pass the full-duplex flag, etc. */
#define MAX_UNITS 8
static int full_duplex[MAX_UNITS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int options[MAX_UNITS] = {-1, -1, -1, -1, -1, -1, -1, -1};

static int epic_open(struct net_device *dev);
static int read_eeprom(long ioaddr, int location);
static int mdio_read(long ioaddr, int phy_id, int location);
static void mdio_write(long ioaddr, int phy_id, int location, int value);
static void epic_restart(struct net_device *dev);
static void epic_timer(unsigned long data);
static void epic_tx_timeout(struct net_device *dev);
static void epic_init_ring(struct net_device *dev);
static int epic_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int epic_rx(struct net_device *dev);
static void epic_interrupt(int irq, void *dev_instance, struct pt_regs *regs);
static int mii_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static int epic_close(struct net_device *dev);
static struct net_device_stats *epic_get_stats(struct net_device *dev);
static void set_rx_mode(struct net_device *dev);




/* Serial EEPROM section. */

/*  EEPROM_Ctrl bits. */
#define EE_SHIFT_CLK	0x04	/* EEPROM shift clock. */
#define EE_CS			0x02	/* EEPROM chip select. */
#define EE_DATA_WRITE	0x08	/* EEPROM chip data in. */
#define EE_WRITE_0		0x01
#define EE_WRITE_1		0x09
#define EE_DATA_READ	0x10	/* EEPROM chip data out. */
#define EE_ENB			(0x0001 | EE_CS)

/* Delay between EEPROM clock transitions.
   No extra delay is needed with 33Mhz PCI, but 66Mhz is untested.
 */

#ifdef _LINUX_DELAY_H
#define eeprom_delay(nanosec)	udelay(1)
#else
#define eeprom_delay(nanosec)	do { ; } while (0)
#endif

/* The EEPROM commands include the alway-set leading bit. */
#define EE_WRITE_CMD	(5 << 6)
#define EE_READ64_CMD	(6 << 6)
#define EE_READ256_CMD	(6 << 8)
#define EE_ERASE_CMD	(7 << 6)

static int read_eeprom(long ioaddr, int location)
{
	int i;
	int retval = 0;
	long ee_addr = ioaddr + EECTL;
	int read_cmd = location |
		(inl(ee_addr) & 0x40) ? EE_READ64_CMD : EE_READ256_CMD;

	outl(EE_ENB & ~EE_CS, ee_addr);
	outl(EE_ENB, ee_addr);

	/* Shift the read command bits out. */
	for (i = 12; i >= 0; i--) {
		short dataval = (read_cmd & (1 << i)) ? EE_WRITE_1 : EE_WRITE_0;
		outl(EE_ENB | dataval, ee_addr);
		eeprom_delay(100);
		outl(EE_ENB | dataval | EE_SHIFT_CLK, ee_addr);
		eeprom_delay(150);
	}
	outl(EE_ENB, ee_addr);

	for (i = 16; i > 0; i--) {
		outl(EE_ENB | EE_SHIFT_CLK, ee_addr);
		eeprom_delay(100);
		retval = (retval << 1) | ((inl(ee_addr) & EE_DATA_READ) ? 1 : 0);
		outl(EE_ENB, ee_addr);
		eeprom_delay(100);
	}

	/* Terminate the EEPROM access. */
	outl(EE_ENB & ~EE_CS, ee_addr);
	return retval;
}

#define MII_READOP		1
#define MII_WRITEOP		2
static int mdio_read(long ioaddr, int phy_id, int location)
{
	int i;

	outl((phy_id << 9) | (location << 4) | MII_READOP, ioaddr + MIICtrl);
	/* Typical operation takes < 50 ticks. */
	for (i = 4000; i > 0; i--)
		if ((inl(ioaddr + MIICtrl) & MII_READOP) == 0)
			return inw(ioaddr + MIIData);
	return 0xffff;
}

static void mdio_write(long ioaddr, int phy_id, int location, int value)
{
	int i;

	outw(value, ioaddr + MIIData);
	outl((phy_id << 9) | (location << 4) | MII_WRITEOP, ioaddr + MIICtrl);
	for (i = 10000; i > 0; i--) {
		if ((inl(ioaddr + MIICtrl) & MII_WRITEOP) == 0)
			break;
	}
	return;
}


static int
epic_open(struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	long ioaddr = dev->base_addr;
	int i;
	int mii_reg5;
	ep->full_duplex = ep->force_fd;

	MOD_INC_USE_COUNT;

	/* Soft reset the chip. */
	outl(0x4001, ioaddr + GENCTL);

	if (request_irq(dev->irq, &epic_interrupt, SA_SHIRQ, EPIC100_MODULE_NAME, dev)) {
		MOD_DEC_USE_COUNT;
		return -EBUSY;
	}

	epic_init_ring(dev);

	outl(0x4000, ioaddr + GENCTL);
	/* This next magic! line by Ken Yamaguchi.. ?? */
	outl(0x0008, ioaddr + TEST1);

	/* Pull the chip out of low-power mode, enable interrupts, and set for
	   PCI read multiple.  The MIIcfg setting and strange write order are
	   required by the details of which bits are reset and the transceiver
	   wiring on the Ositech CardBus card.
	*/
	outl(0x12, ioaddr + MIICfg);
	if (ep->chip_id == 6)
		outl((inl(ioaddr + NVCTL) & ~0x003C) | 0x4800, ioaddr + NVCTL);

#if defined(__powerpc__) || defined(__sparc__)		/* Big endian */
	outl(0x0432 | (RX_FIFO_THRESH<<8), ioaddr + GENCTL);
#else
	outl(0x0412 | (RX_FIFO_THRESH<<8), ioaddr + GENCTL);
#endif

	for (i = 0; i < 3; i++)
		outl(((u16*)dev->dev_addr)[i], ioaddr + LAN0 + i*4);

	outl(TX_FIFO_THRESH, ioaddr + TxThresh);

	mii_reg5 = mdio_read(ioaddr, ep->phys[0], 5);
	if (mii_reg5 != 0xffff) {
		if ((mii_reg5 & 0x0100) || (mii_reg5 & 0x01C0) == 0x0040)
			ep->full_duplex = 1;
		else if (! (mii_reg5 & 0x4000))
			mdio_write(ioaddr, ep->phys[0], 0, 0x1200);
		if (epic_debug > 1)
			printk(KERN_INFO "%s: Setting %s-duplex based on MII xcvr %d"
				   " register read of %4.4x.\n", dev->name,
				   ep->full_duplex ? "full" : "half", ep->phys[0], mii_reg5);
	}

	outl(ep->full_duplex ? 0x7F : 0x79, ioaddr + TxCtrl);
	outl(virt_to_bus(ep->rx_ring), ioaddr + PRxCDAR);
	outl(virt_to_bus(ep->tx_ring), ioaddr + PTxCDAR);

	/* Start the chip's Rx process. */
	set_rx_mode(dev);
	outl(0x000A, ioaddr + COMMAND);

	/* Enable interrupts by setting the interrupt mask. */
	outl((ep->chip_id == 6 ? PCIBusErr175 : PCIBusErr170)
		 | CntFull | TxUnderrun | TxDone
		 | RxError | RxOverflow | RxFull | RxHeader | RxDone,
		 ioaddr + INTMASK);

	if (epic_debug > 1)
		printk(KERN_DEBUG "%s: epic_open() ioaddr %lx IRQ %d status %4.4x "
			   "%s-duplex.\n",
			   dev->name, ioaddr, dev->irq, inl(ioaddr + GENCTL),
			   ep->full_duplex ? "full" : "half");

	netif_start_queue(dev);

	/* Set the timer to switch to check for link beat and perhaps switch
	   to an alternate media type. */
	init_timer(&ep->timer);
	ep->timer.expires = RUN_AT(3*HZ);			/* 3 sec. */
	ep->timer.data = (unsigned long)dev;
	ep->timer.function = &epic_timer;				/* timer handler */
	add_timer(&ep->timer);

	return 0;
}


/* Reset the chip to recover from a PCI transaction error.
   This may occur at interrupt time. */
static void epic_pause(struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct epic_private *ep = (struct epic_private *)dev->priv;

	netif_stop_queue (dev);
	
	/* Disable interrupts by clearing the interrupt mask. */
	outl(0x00000000, ioaddr + INTMASK);
	/* Stop the chip's Tx and Rx DMA processes. */
	outw(0x0061, ioaddr + COMMAND);

	/* Update the error counts. */
	if (inw(ioaddr + COMMAND) != 0xffff) {
		ep->stats.rx_missed_errors += inb(ioaddr + MPCNT);
		ep->stats.rx_frame_errors += inb(ioaddr + ALICNT);
		ep->stats.rx_crc_errors += inb(ioaddr + CRCCNT);
	}

	/* Remove the packets on the Rx queue. */
	epic_rx(dev);
}

static void epic_restart(struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct epic_private *ep = (struct epic_private *)dev->priv;
	int i;

	printk(KERN_DEBUG "%s: Restarting the EPIC chip, Rx %d/%d Tx %d/%d.\n",
		   dev->name, ep->cur_rx, ep->dirty_rx, ep->dirty_tx, ep->cur_tx);
	/* Soft reset the chip. */
	outl(0x0001, ioaddr + GENCTL);

	udelay(1);
	/* Duplicate code from epic_open(). */
	outl(0x0008, ioaddr + TEST1);

#if defined(__powerpc__)		/* Big endian */
	outl(0x0432 | (RX_FIFO_THRESH<<8), ioaddr + GENCTL);
#else
	outl(0x0412 | (RX_FIFO_THRESH<<8), ioaddr + GENCTL);
#endif
	outl(0x12, ioaddr + MIICfg);
	if (ep->chip_id == 6)
		outl((inl(ioaddr + NVCTL) & ~0x003C) | 0x4800, ioaddr + NVCTL);

	for (i = 0; i < 3; i++)
		outl(((u16*)dev->dev_addr)[i], ioaddr + LAN0 + i*4);

	outl(TX_FIFO_THRESH, ioaddr + TxThresh);
	outl(ep->full_duplex ? 0x7F : 0x79, ioaddr + TxCtrl);
	outl(virt_to_bus(&ep->rx_ring[ep->cur_rx%RX_RING_SIZE]), ioaddr + PRxCDAR);
	outl(virt_to_bus(&ep->tx_ring[ep->dirty_tx%TX_RING_SIZE]),
		 ioaddr + PTxCDAR);

	/* Start the chip's Rx process. */
	set_rx_mode(dev);
	outl(0x000A, ioaddr + COMMAND);

	/* Enable interrupts by setting the interrupt mask. */
	outl((ep->chip_id == 6 ? PCIBusErr175 : PCIBusErr170)
		 | CntFull | TxUnderrun | TxDone
		 | RxError | RxOverflow | RxFull | RxHeader | RxDone,
		 ioaddr + INTMASK);

	netif_start_queue (dev);
	
	printk(KERN_DEBUG "%s: epic_restart() done, cmd status %4.4x, ctl %4.4x"
		   " interrupt %4.4x.\n",
			   dev->name, inl(ioaddr + COMMAND), inl(ioaddr + GENCTL),
		   inl(ioaddr + INTSTAT));
}

static void epic_timer(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct epic_private *ep = (struct epic_private *)dev->priv;
	long ioaddr = dev->base_addr;
	int next_tick = 0;
	int mii_reg5 = mdio_read(ioaddr, ep->phys[0], 5);

	if (epic_debug > 3) {
		printk(KERN_DEBUG "%s: Media selection tick, Tx status %8.8x.\n",
			   dev->name, inl(ioaddr + TxSTAT));
		printk(KERN_DEBUG "%s: Other registers are IntMask %4.4x "
			   "IntStatus %4.4x RxStatus %4.4x.\n",
			   dev->name, inl(ioaddr + INTMASK), inl(ioaddr + INTSTAT),
			   inl(ioaddr + RxSTAT));
	}
	if (! ep->force_fd  &&  mii_reg5 != 0xffff) {
		int duplex = (mii_reg5&0x0100) || (mii_reg5 & 0x01C0) == 0x0040;
		if (ep->full_duplex != duplex) {
			ep->full_duplex = duplex;
			printk(KERN_INFO "%s: Setting %s-duplex based on MII #%d link"
				   " partner capability of %4.4x.\n", dev->name,
				   ep->full_duplex ? "full" : "half", ep->phys[0], mii_reg5);
			outl(ep->full_duplex ? 0x7F : 0x79, ioaddr + TxCtrl);
		}
		next_tick = 60*HZ;
	}

	if (next_tick) {
		ep->timer.expires = RUN_AT(next_tick);
		add_timer(&ep->timer);
	}
}

static void epic_tx_timeout(struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	long ioaddr = dev->base_addr;

	if (epic_debug > 0) {
		printk(KERN_WARNING "%s: Transmit timeout using MII device, "
			   "Tx status %4.4x.\n",
			   dev->name, inw(ioaddr + TxSTAT));
		if (epic_debug > 1) {
			printk(KERN_DEBUG "%s: Tx indices: dirty_tx %d, cur_tx %d.\n",
			 dev->name, ep->dirty_tx, ep->cur_tx);
		}
	}
	if (inw(ioaddr + TxSTAT) & 0x10) {		/* Tx FIFO underflow. */
		ep->stats.tx_fifo_errors++;
		/* Restart the transmit process. */
		outl(0x0080, ioaddr + COMMAND);
	}

	/* Perhaps stop and restart the chip's Tx processes . */
	/* Trigger a transmit demand. */
	outl(0x0004, dev->base_addr + COMMAND);

	dev->trans_start = jiffies;
	ep->stats.tx_errors++;
	
	netif_start_queue (dev);
}

/* Initialize the Rx and Tx rings, along with various 'dev' bits. */
static void
epic_init_ring(struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	int i;

	ep->tx_full = 0;
	ep->cur_rx = ep->cur_tx = 0;
	ep->dirty_rx = ep->dirty_tx = 0;

	for (i = 0; i < RX_RING_SIZE; i++) {
		ep->rx_ring[i].status = 0x8000;		/* Owned by Epic chip */
		ep->rx_ring[i].buflength = PKT_BUF_SZ;
		{
			/* Note the receive buffer must be longword aligned.
			   dev_alloc_skb() provides 16 byte alignment.  But do *not*
			   use skb_reserve() to align the IP header! */
			struct sk_buff *skb;
			skb = dev_alloc_skb(PKT_BUF_SZ);
			ep->rx_skbuff[i] = skb;
			if (skb == NULL)
				break;			/* Bad news!  */
			skb->dev = dev;			/* Mark as being used by this device. */
			skb_reserve(skb, 2); /* Align IP on 16 byte boundaries */
			ep->rx_ring[i].bufaddr = virt_to_bus(skb->tail);
		}
		ep->rx_ring[i].next = virt_to_bus(&ep->rx_ring[i+1]);
	}
	/* Mark the last entry as wrapping the ring. */
	ep->rx_ring[i-1].next = virt_to_bus(&ep->rx_ring[0]);

	/* The Tx buffer descriptor is filled in as needed, but we
	   do need to clear the ownership bit. */
	for (i = 0; i < TX_RING_SIZE; i++) {
		ep->tx_skbuff[i] = 0;
		ep->tx_ring[i].status = 0x0000;
		ep->tx_ring[i].next = virt_to_bus(&ep->tx_ring[i+1]);
	}
	ep->tx_ring[i-1].next = virt_to_bus(&ep->tx_ring[0]);
}

static int
epic_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	int entry;
	u32 flag;

	netif_stop_queue (dev);

	/* Caution: the write order is important here, set the base address
	   with the "ownership" bits last. */

	/* Calculate the next Tx descriptor entry. */
	entry = ep->cur_tx % TX_RING_SIZE;

	ep->tx_skbuff[entry] = skb;
	ep->tx_ring[entry].txlength = (skb->len >= ETH_ZLEN ? skb->len : ETH_ZLEN);
	ep->tx_ring[entry].bufaddr = virt_to_bus(skb->data);
	ep->tx_ring[entry].buflength = skb->len;

	/* tx_bytes counting -- Nolan Leake */	
	ep->stats.tx_bytes += ep->tx_ring[entry].txlength;
	
	if (ep->cur_tx - ep->dirty_tx < TX_RING_SIZE/2) {/* Typical path */
	  flag = 0x10; /* No interrupt */
	} else if (ep->cur_tx - ep->dirty_tx == TX_RING_SIZE/2) {
	  flag = 0x14; /* Tx-done intr. */
	} else if (ep->cur_tx - ep->dirty_tx < TX_RING_SIZE - 2) {
	  flag = 0x10; /* No Tx-done intr. */
	} else {
	  /* Leave room for two additional entries. */
	  flag = 0x14; /* Tx-done intr. */
	  ep->tx_full = 1;
	}

	ep->tx_ring[entry].control = flag;
	ep->tx_ring[entry].status = 0x8000;	/* Pass ownership to the chip. */
	ep->cur_tx++;
	/* Trigger an immediate transmit demand. */
	outl(0x0004, dev->base_addr + COMMAND);

	dev->trans_start = jiffies;

	if (! ep->tx_full)
		netif_start_queue (dev);

	if (epic_debug > 4)
		printk(KERN_DEBUG "%s: Queued Tx packet size %d to slot %d, "
			   "flag %2.2x Tx status %8.8x.\n",
			   dev->name, (int)skb->len, entry, flag,
			   inl(dev->base_addr + TxSTAT));

	return 0;
}

/* The interrupt handler does all of the Rx thread work and cleans up
   after the Tx thread. */
static void epic_interrupt(int irq, void *dev_instance, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *)dev_instance;
	struct epic_private *ep = (struct epic_private *)dev->priv;
	long ioaddr = dev->base_addr;
	int status, boguscnt = max_interrupt_work;

	spin_lock (&ep->lock);

	do {
		status = inl(ioaddr + INTSTAT);
		/* Acknowledge all of the current interrupt sources ASAP. */
		outl(status & 0x00007fff, ioaddr + INTSTAT);

		if (epic_debug > 4)
			printk("%s: interrupt  interrupt=%#8.8x new intstat=%#8.8x.\n",
				   dev->name, status, inl(ioaddr + INTSTAT));

		if ((status & IntrSummary) == 0)
			break;

		if (status & (RxDone | RxStarted | RxEarlyWarn))
			epic_rx(dev);

		if (status & (TxEmpty | TxDone)) {
			int dirty_tx;

			for (dirty_tx = ep->dirty_tx; dirty_tx < ep->cur_tx; dirty_tx++) {
				int entry = dirty_tx % TX_RING_SIZE;
				int txstatus = ep->tx_ring[entry].status;

				if (txstatus < 0)
					break;			/* It still hasn't been Txed */

				if ( ! (txstatus & 0x0001)) {
					/* There was an major error, log it. */
#ifndef final_version
					if (epic_debug > 1)
						printk("%s: Transmit error, Tx status %8.8x.\n",
							   dev->name, txstatus);
#endif
					ep->stats.tx_errors++;
					if (txstatus & 0x1050) ep->stats.tx_aborted_errors++;
					if (txstatus & 0x0008) ep->stats.tx_carrier_errors++;
					if (txstatus & 0x0040) ep->stats.tx_window_errors++;
					if (txstatus & 0x0010) ep->stats.tx_fifo_errors++;
#ifdef ETHER_STATS
					if (txstatus & 0x1000) ep->stats.collisions16++;
#endif
				} else {
#ifdef ETHER_STATS
					if ((txstatus & 0x0002) != 0) ep->stats.tx_deferred++;
#endif
					ep->stats.collisions += (txstatus >> 8) & 15;
					ep->stats.tx_packets++;
				}

				/* Free the original skb. */
				dev_kfree_skb_irq(ep->tx_skbuff[entry]);
				ep->tx_skbuff[entry] = 0;
			}

#ifndef final_version
			if (ep->cur_tx - dirty_tx > TX_RING_SIZE) {
				printk("%s: Out-of-sync dirty pointer, %d vs. %d, full=%d.\n",
					   dev->name, dirty_tx, ep->cur_tx, ep->tx_full);
				dirty_tx += TX_RING_SIZE;
			}
#endif

			if (ep->tx_full &&
			    netif_queue_stopped(dev) &&
			    dirty_tx > ep->cur_tx - TX_RING_SIZE + 2) {
				/* The ring is no longer full, clear tbusy. */
				ep->tx_full = 0;
			}
			
			if (ep->tx_full)
				netif_stop_queue (dev);
			else
				netif_wake_queue (dev);

			ep->dirty_tx = dirty_tx;
		}

		/* Check uncommon events all at once. */
		if (status & (CntFull | TxUnderrun | RxOverflow |
					  PCIBusErr170 | PCIBusErr175)) {
			if (status == 0xffffffff) /* Chip failed or removed (CardBus). */
				break;
			/* Always update the error counts to avoid overhead later. */
			ep->stats.rx_missed_errors += inb(ioaddr + MPCNT);
			ep->stats.rx_frame_errors += inb(ioaddr + ALICNT);
			ep->stats.rx_crc_errors += inb(ioaddr + CRCCNT);

			if (status & TxUnderrun) { /* Tx FIFO underflow. */
				ep->stats.tx_fifo_errors++;
				outl(1536, ioaddr + TxThresh);
				/* Restart the transmit process. */
				outl(0x0080, ioaddr + COMMAND);
			}
			if (status & RxOverflow) {		/* Missed a Rx frame. */
				ep->stats.rx_errors++;
			}
			if (status & PCIBusErr170) {
				printk(KERN_ERR "%s: PCI Bus Error!  EPIC status %4.4x.\n",
					   dev->name, status);
				epic_pause(dev);
				epic_restart(dev);
			}
			/* Clear all error sources. */
			outl(status & 0x7f18, ioaddr + INTSTAT);
		}
		if (--boguscnt < 0) {
			printk(KERN_ERR "%s: Too much work at interrupt, "
				   "IntrStatus=0x%8.8x.\n",
				   dev->name, status);
			/* Clear all interrupt sources. */
			outl(0x0001ffff, ioaddr + INTSTAT);
			break;
		}
	} while (1);

	if (epic_debug > 3)
		printk(KERN_DEBUG "%s: exiting interrupt, intr_status=%#4.4x.\n",
			   dev->name, inl(ioaddr + INTSTAT));

	spin_unlock (&ep->lock);
}


static int epic_rx(struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	int entry = ep->cur_rx % RX_RING_SIZE;
	int work_done = 0;

	if (epic_debug > 4)
		printk(KERN_DEBUG " In epic_rx(), entry %d %8.8x.\n", entry,
			   ep->rx_ring[entry].status);
	/* If we own the next entry, it's a new packet. Send it up. */
	while (ep->rx_ring[entry].status >= 0  &&  ep->rx_skbuff[entry]) {
		int status = ep->rx_ring[entry].status;

		if (epic_debug > 4)
			printk(KERN_DEBUG "  epic_rx() status was %8.8x.\n", status);
		if (status & 0x2006) {
			if (status & 0x2000) {
				printk(KERN_WARNING "%s: Oversized Ethernet frame spanned "
					   "multiple buffers, status %4.4x!\n", dev->name, status);
				ep->stats.rx_length_errors++;
			} else if (status & 0x0006)
				/* Rx Frame errors are counted in hardware. */
				ep->stats.rx_errors++;
		} else {
			/* Malloc up new buffer, compatible with net-2e. */
			/* Omit the four octet CRC from the length. */
			short pkt_len = ep->rx_ring[entry].rxlength - 4;
			struct sk_buff *skb;

			/* Check if the packet is long enough to accept without copying
			   to a minimally-sized skbuff. */
			if (pkt_len < rx_copybreak
				&& (skb = dev_alloc_skb(pkt_len + 2)) != NULL) {
				skb->dev = dev;
				skb_reserve(skb, 2);	/* 16 byte align the IP header */
#if 1 /* USE_IP_COPYSUM */
				eth_copy_and_sum(skb, bus_to_virt(ep->rx_ring[entry].bufaddr),
								 pkt_len, 0);
				skb_put(skb, pkt_len);
#else
				memcpy(skb_put(skb, pkt_len),
					   bus_to_virt(ep->rx_ring[entry].bufaddr), pkt_len);
#endif
				ep->rx_ring[entry].status = 0x8000;
			} else {
				skb_put(skb = ep->rx_skbuff[entry], pkt_len);
				ep->rx_skbuff[entry] = NULL;
			}
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
			ep->stats.rx_packets++;
			/* rx_bytes counting -- Nolan Leake */
			ep->stats.rx_bytes += pkt_len;
		}
		work_done++;
		entry = (++ep->cur_rx) % RX_RING_SIZE;
	}

	/* Refill the Rx ring buffers. */
	for (; ep->cur_rx - ep->dirty_rx > 0; ep->dirty_rx++) {
		entry = ep->dirty_rx % RX_RING_SIZE;
		if (ep->rx_skbuff[entry] == NULL) {
			struct sk_buff *skb;
			skb = ep->rx_skbuff[entry] = dev_alloc_skb(PKT_BUF_SZ);
			if (skb == NULL)
				break;
			skb->dev = dev;			/* Mark as being used by this device. */
			skb_reserve(skb, 2);	/* Align IP on 16 byte boundaries */
			ep->rx_ring[entry].bufaddr = virt_to_bus(skb->tail);
			work_done++;
		}
		ep->rx_ring[entry].status = 0x8000;
	}
	return work_done;
}


static int epic_close(struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct epic_private *ep = (struct epic_private *)dev->priv;
	int i;

	netif_stop_queue(dev);

	if (epic_debug > 1)
		printk(KERN_DEBUG "%s: Shutting down ethercard, status was %2.2x.\n",
			   dev->name, inl(ioaddr + INTSTAT));

	/* Disable interrupts by clearing the interrupt mask. */
	outl(0x00000000, ioaddr + INTMASK);
	/* Stop the chip's Tx and Rx DMA processes. */
	outw(0x0061, ioaddr + COMMAND);

	/* Update the error counts. */
	ep->stats.rx_missed_errors += inb(ioaddr + MPCNT);
	ep->stats.rx_frame_errors += inb(ioaddr + ALICNT);
	ep->stats.rx_crc_errors += inb(ioaddr + CRCCNT);

	del_timer(&ep->timer);

	free_irq(dev->irq, dev);

	/* Free all the skbuffs in the Rx queue. */
	for (i = 0; i < RX_RING_SIZE; i++) {
		struct sk_buff *skb = ep->rx_skbuff[i];
		ep->rx_skbuff[i] = 0;
		ep->rx_ring[i].status = 0;		/* Not owned by Epic chip. */
		ep->rx_ring[i].buflength = 0;
		ep->rx_ring[i].bufaddr = 0xBADF00D0; /* An invalid address. */
		if (skb) {
			dev_kfree_skb(skb);
		}
	}
	for (i = 0; i < TX_RING_SIZE; i++) {
		if (ep->tx_skbuff[i])
			dev_kfree_skb(ep->tx_skbuff[i]);
		ep->tx_skbuff[i] = 0;
	}


	/* Green! Leave the chip in low-power mode. */
	outl(0x0008, ioaddr + GENCTL);

	MOD_DEC_USE_COUNT;

	return 0;
}


static struct net_device_stats *epic_get_stats(struct net_device *dev)
{
	struct epic_private *ep = (struct epic_private *)dev->priv;
	long ioaddr = dev->base_addr;

	if (netif_running(dev)) {
		/* Update the error counts. */
		ep->stats.rx_missed_errors += inb(ioaddr + MPCNT);
		ep->stats.rx_frame_errors += inb(ioaddr + ALICNT);
		ep->stats.rx_crc_errors += inb(ioaddr + CRCCNT);
	}

	return &ep->stats;
}


/* Set or clear the multicast filter for this adaptor.
   Note that we only use exclusion around actually queueing the
   new frame, not around filling ep->setup_frame.  This is non-deterministic
   when re-entered but still correct. */

/* The little-endian AUTODIN II ethernet CRC calculation.
   N.B. Do not use for bulk data, use a table-based routine instead.
   This is common code and should be moved to net/core/crc.c */
static unsigned const ethernet_polynomial_le = 0xedb88320U;
static inline unsigned ether_crc_le(int length, unsigned char *data)
{
	unsigned int crc = 0xffffffff;	/* Initial value. */
	while(--length >= 0) {
		unsigned char current_octet = *data++;
		int bit;
		for (bit = 8; --bit >= 0; current_octet >>= 1) {
			if ((crc ^ current_octet) & 1) {
				crc >>= 1;
				crc ^= ethernet_polynomial_le;
			} else
				crc >>= 1;
		}
	}
	return crc;
}


static void set_rx_mode(struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct epic_private *ep = (struct epic_private *)dev->priv;
	unsigned char mc_filter[8];		 /* Multicast hash filter */
	int i;

	if (dev->flags & IFF_PROMISC) {			/* Set promiscuous. */
		outl(0x002C, ioaddr + RxCtrl);
		/* Unconditionally log net taps. */
		printk(KERN_INFO "%s: Promiscuous mode enabled.\n", dev->name);
		memset(mc_filter, 0xff, sizeof(mc_filter));
	} else if ((dev->mc_count > 0)  ||  (dev->flags & IFF_ALLMULTI)) {
		/* There is apparently a chip bug, so the multicast filter
		   is never enabled. */
		/* Too many to filter perfectly -- accept all multicasts. */
		memset(mc_filter, 0xff, sizeof(mc_filter));
		outl(0x000C, ioaddr + RxCtrl);
	} else if (dev->mc_count == 0) {
		outl(0x0004, ioaddr + RxCtrl);
		return;
	} else {					/* Never executed, for now. */
		struct dev_mc_list *mclist;

		memset(mc_filter, 0, sizeof(mc_filter));
		for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
			 i++, mclist = mclist->next)
			set_bit(ether_crc_le(ETH_ALEN, mclist->dmi_addr) & 0x3f,
					mc_filter);
	}
	/* ToDo: perhaps we need to stop the Tx and Rx process here? */
	if (memcmp(mc_filter, ep->mc_filter, sizeof(mc_filter))) {
		for (i = 0; i < 4; i++)
			outw(((u16 *)mc_filter)[i], ioaddr + MC0 + i*4);
		memcpy(ep->mc_filter, mc_filter, sizeof(mc_filter));
	}
	return;
}


static int mii_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	long ioaddr = dev->base_addr;
	u16 *data = (u16 *)&rq->ifr_data;

	switch(cmd) {
	case SIOCDEVPRIVATE:		/* Get the address of the PHY in use. */
		data[0] = ((struct epic_private *)dev->priv)->phys[0] & 0x1f;
		/* Fall Through */
	case SIOCDEVPRIVATE+1:		/* Read the specified MII register. */
		if (! netif_running(dev)) {
			outl(0x0200, ioaddr + GENCTL);
			outl((inl(ioaddr + NVCTL) & ~0x003C) | 0x4800, ioaddr + NVCTL);
		}
		data[3] = mdio_read(ioaddr, data[0] & 0x1f, data[1] & 0x1f);
		if (! netif_running(dev)) {
#ifdef notdef
			outl(0x0008, ioaddr + GENCTL);
			outl((inl(ioaddr + NVCTL) & ~0x483C) | 0x0000, ioaddr + NVCTL);
#endif
		}
		return 0;
	case SIOCDEVPRIVATE+2:		/* Write the specified MII register */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		if (! netif_running(dev)) {
			outl(0x0200, ioaddr + GENCTL);
			outl((inl(ioaddr + NVCTL) & ~0x003C) | 0x4800, ioaddr + NVCTL);
		}
		mdio_write(ioaddr, data[0] & 0x1f, data[1] & 0x1f, data[2]);
		if (! netif_running(dev)) {
#ifdef notdef
			outl(0x0008, ioaddr + GENCTL);
			outl((inl(ioaddr + NVCTL) & ~0x483C) | 0x0000, ioaddr + NVCTL);
#endif
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}


static int __devinit epic100_init_one (struct pci_dev *pdev,
				       const struct pci_device_id *ent)
{
	struct epic_private *ep;
	int i, option = 0, duplex = 0, irq, chip_idx;
	struct net_device *dev;
	long ioaddr;
	static int card_idx = -1;
	static int printed_version = 0;
	
	if (!printed_version) {
		printk (KERN_INFO "%s", version);
		printed_version = 1;
	}
	
	chip_idx = ent->driver_data;
	
	ioaddr = pci_resource_start (pdev, 0);
	if (!ioaddr)
		return -ENODEV;
	
	irq = pdev->irq;
	if (!irq)
		return -ENODEV;

	/* We do a request_region() to register /proc/ioports info. */
	if (!request_region(ioaddr, EPIC_TOTAL_SIZE, EPIC100_MODULE_NAME))
		return -EBUSY;

	pci_enable_device (pdev);

	/* EPIC-specific code: Soft-reset the chip ere setting as master. */
	outl(0x0001, ioaddr + GENCTL);
	
	pci_set_master (pdev);

// FIXME	if (dev && dev->mem_start) {
//		option = dev->mem_start;
//		duplex = (dev->mem_start & 16) ? 1 : 0;
//	}
//	else 

	card_idx++;
	if (card_idx >= 0  &&  card_idx < MAX_UNITS) {
		if (options[card_idx] >= 0)
			option = options[card_idx];
		if (full_duplex[card_idx] >= 0)
			duplex = full_duplex[card_idx];
	}

	dev = init_etherdev(NULL, sizeof (*ep));
	if (!dev)
		goto err_out_free_region;

	dev->base_addr = ioaddr;
	dev->irq = irq;
	printk(KERN_INFO "%s: %s at %#lx, IRQ %d, ",
		   dev->name, epic100_chip_info[chip_idx].name,
		   ioaddr, dev->irq);

	/* Bring the chip out of low-power mode. */
	outl(0x4200, ioaddr + GENCTL);
	/* Magic?!  If we don't set this bit the MII interface won't work. */
	outl(0x0008, ioaddr + TEST1);

	/* Turn on the MII transceiver. */
	outl(0x12, ioaddr + MIICfg);
	if (chip_idx == 1)
		outl((inl(ioaddr + NVCTL) & ~0x003C) | 0x4800, ioaddr + NVCTL);
	outl(0x0200, ioaddr + GENCTL);

	/* This could also be read from the EEPROM. */
	for (i = 0; i < 3; i++)
		((u16 *)dev->dev_addr)[i] = inw(ioaddr + LAN0 + i*4);

	for (i = 0; i < 5; i++)
		printk("%2.2x:", dev->dev_addr[i]);
	printk("%2.2x.\n", dev->dev_addr[i]);

	if (epic_debug > 1) {
		printk(KERN_DEBUG "%s: EEPROM contents\n", dev->name);
		for (i = 0; i < 64; i++)
			printk(" %4.4x%s", read_eeprom(ioaddr, i),
				   i % 16 == 15 ? "\n" : "");
	}

	/* The data structures must be quadword aligned,
	 * init_etherdev ensures this */
	ep = dev->priv;
	memset(ep, 0, sizeof(*ep));

	ep->lock = SPIN_LOCK_UNLOCKED;
	ep->chip_id = pdev->device;
	ep->pdev = pdev;
	pdev->driver_data = dev;

	/* Find the connected MII xcvrs.
	   Doing this in open() would allow detecting external xcvrs later, but
	   takes too much time. */
	{
		int phy, phy_idx;
		for (phy = 1, phy_idx = 0; phy < 32 && phy_idx < sizeof(ep->phys);
			 phy++) {
			int mii_status = mdio_read(ioaddr, phy, 1);
			if (mii_status != 0xffff  && mii_status != 0x0000) {
				ep->phys[phy_idx++] = phy;
				printk(KERN_INFO "%s: MII transceiver #%d control "
					   "%4.4x status %4.4x.\n"
					   KERN_INFO "%s:  Autonegotiation advertising %4.4x "
					   "link partner %4.4x.\n",
					   dev->name, phy, mdio_read(ioaddr, phy, 0), mii_status,
					   dev->name, mdio_read(ioaddr, phy, 4),
					   mdio_read(ioaddr, phy, 5));
			}
		}
		if (phy_idx == 0) {
			printk(KERN_WARNING "%s: ***WARNING***: No MII transceiver found!\n",
				   dev->name);
			/* Use the known PHY address of the EPII. */
			ep->phys[0] = 3;
		}
	}

	/* Turn off the MII xcvr (175 only!), leave the chip in low-power mode. */
	if (ep->chip_id == 6)
		outl(inl(ioaddr + NVCTL) & ~0x483C, ioaddr + NVCTL);
	outl(0x0008, ioaddr + GENCTL);

	/* The lower four bits are the media type. */
	ep->force_fd = duplex;
	ep->default_port = option;
	if (ep->default_port)
		ep->medialock = 1;

	/* The Epic-specific entries in the device structure. */
	dev->open = epic_open;
	dev->hard_start_xmit = epic_start_xmit;
	dev->stop = epic_close;
	dev->get_stats = epic_get_stats;
	dev->set_multicast_list = set_rx_mode;
	dev->do_ioctl = mii_ioctl;
	dev->tx_timeout = epic_tx_timeout;
	dev->watchdog_timeo = TX_TIMEOUT;
	
	netif_stop_queue (dev);

	return 0;

err_out_free_region:
	release_region(ioaddr, EPIC_TOTAL_SIZE);
	return -ENODEV;
}


static void __devexit epic100_remove_one (struct pci_dev *pdev)
{
	struct net_device *dev = pdev->driver_data;

	unregister_netdev(dev);
	release_region(dev->base_addr, EPIC_TOTAL_SIZE);
	kfree(dev);
}


static void epic100_suspend (struct pci_dev *pdev)
{
	struct net_device *dev = pdev->driver_data;
	long ioaddr = dev->base_addr;

	epic_pause(dev);
	/* Put the chip into low-power mode. */
	outl(0x0008, ioaddr + GENCTL);
}


static void epic100_resume (struct pci_dev *pdev)
{
	struct net_device *dev = pdev->driver_data;

	epic_restart (dev);
}


static struct pci_driver epic100_driver = {
	name:		EPIC100_MODULE_NAME,
	id_table:	epic100_pci_tbl,
	probe:		epic100_init_one,
	remove:		epic100_remove_one,
	suspend:	epic100_suspend,
	resume:		epic100_resume,
};


static int __init epic100_init (void)
{
	return pci_module_init (&epic100_driver);
}


static void __exit epic100_cleanup (void)
{
	pci_unregister_driver (&epic100_driver);
}


module_init(epic100_init);
module_exit(epic100_cleanup);


/*
 * Local variables:
 *  compile-command: "gcc -DMODULE -D__KERNEL__ -Wall -Wstrict-prototypes -O6 -c epic100.c `[ -f /usr/include/linux/modversions.h ] && echo -DMODVERSIONS`"
 *  cardbus-compile-command: "gcc -DCARDBUS -DMODULE -D__KERNEL__ -Wall -Wstrict-prototypes -O6 -c epic100.c -o epic_cb.o -I/usr/src/pcmcia-cs-3.0.5/include/"
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
