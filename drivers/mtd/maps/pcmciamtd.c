/*
 * $Id: pcmciamtd.c,v 1.36 2002/10/14 18:49:12 rmk Exp $
 *
 * pcmciamtd.c - MTD driver for PCMCIA flash memory cards
 *
 * Author: Simon Evans <spse@secret.org.uk>
 *
 * Copyright (C) 2002 Simon Evans
 *
 * Licence: GPL
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <asm/io.h>
#include <asm/system.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#include <linux/mtd/map.h>

#ifdef CONFIG_MTD_DEBUG
static int debug = CONFIG_MTD_DEBUG_VERBOSE;
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Set Debug Level 0=quiet, 5=noisy");
#undef DEBUG
#define DEBUG(n, format, arg...) \
	if (n <= debug) {	 \
		printk(KERN_DEBUG __FILE__ ":%s(): " format "\n", __FUNCTION__ , ## arg); \
	}

#else
#undef DEBUG
#define DEBUG(n, arg...)
static const int debug = 0;
#endif

#define err(format, arg...) printk(KERN_ERR __FILE__ ": " format "\n" , ## arg)
#define info(format, arg...) printk(KERN_INFO __FILE__ ": " format "\n" , ## arg)
#define warn(format, arg...) printk(KERN_WARNING __FILE__ ": " format "\n" , ## arg)


#define DRIVER_DESC	"PCMCIA Flash memory card driver"
#define DRIVER_VERSION	"$Revision: 1.36 $"

/* Size of the PCMCIA address space: 26 bits = 64 MB */
#define MAX_PCMCIA_ADDR	0x4000000

struct pcmciamtd_dev {
	struct list_head list;
	dev_link_t	link;		/* PCMCIA link */
	caddr_t		win_base;	/* ioremapped address of PCMCIA window */
	unsigned int	win_size;	/* size of window */
	unsigned int	cardsize;	/* size of whole card */
	unsigned int	offset;		/* offset into card the window currently points at */
	struct map_info	pcmcia_map;
	struct mtd_info	*mtd_info;
	u8		vpp;
	char		mtd_name[sizeof(struct cistpl_vers_1_t)];
};


static dev_info_t dev_info = "pcmciamtd";
static LIST_HEAD(dev_list);

/* Module parameters */

/* 2 = do 16-bit transfers, 1 = do 8-bit transfers */
static int buswidth = 2;

/* Speed of memory accesses, in ns */
static int mem_speed;

/* Force the size of an SRAM card */
static int force_size;

/* Force Vpp */
static int vpp;

/* Set Vpp */
static int setvpp;

/* Force card to be treated as FLASH, ROM or RAM */
static int mem_type;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Evans <spse@secret.org.uk>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_PARM(buswidth, "i");
MODULE_PARM_DESC(buswidth, "Set buswidth (1=8 bit, 2=16 bit, default=2)");
MODULE_PARM(mem_speed, "i");
MODULE_PARM_DESC(mem_speed, "Set memory access speed in ns");
MODULE_PARM(force_size, "i");
MODULE_PARM_DESC(force_size, "Force size of card in MB (1-64)");
MODULE_PARM(setvpp, "i");
MODULE_PARM_DESC(setvpp, "Set Vpp (0=Never, 1=On writes, 2=Always on, default=0)");
MODULE_PARM(vpp, "i");
MODULE_PARM_DESC(vpp, "Vpp value in 1/10ths eg 33=3.3V 120=12V (Dangerous)");
MODULE_PARM(mem_type, "i");
MODULE_PARM_DESC(mem_type, "Set Memory type (0=Flash, 1=RAM, 2=ROM, default=0)");



static void inline cs_error(client_handle_t handle, int func, int ret)
{
	error_info_t err = { func, ret };
	CardServices(ReportError, handle, &err);
}


/* read/write{8,16} copy_{from,to} routines with window remapping to access whole card */

static caddr_t remap_window(struct map_info *map, unsigned long to)
{
	struct pcmciamtd_dev *dev = (struct pcmciamtd_dev *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;
	memreq_t mrq;
	int ret;

	mrq.CardOffset = to & ~(dev->win_size-1);
	if(mrq.CardOffset != dev->offset) {
		DEBUG(2, "Remapping window from 0x%8.8x to 0x%8.8x",
		      dev->offset, mrq.CardOffset);
		mrq.Page = 0;
		if( (ret = CardServices(MapMemPage, win, &mrq)) != CS_SUCCESS) {
			cs_error(dev->link.handle, MapMemPage, ret);
			return NULL;
		}
		dev->offset = mrq.CardOffset;
	}
	return dev->win_base + (to & (dev->win_size-1));
}


static u8 pcmcia_read8_remap(struct map_info *map, unsigned long ofs)
{
	caddr_t addr;
	u8 d;

	addr = remap_window(map, ofs);
	if(!addr)
		return 0;

	d = readb(addr);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x", ofs, addr, d);
	return d;
}


static u16 pcmcia_read16_remap(struct map_info *map, unsigned long ofs)
{
	caddr_t addr;
	u16 d;

	addr = remap_window(map, ofs);
	if(!addr)
		return 0;

	d = readw(addr);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x", ofs, addr, d);
	return d;
}


static void pcmcia_copy_from_remap(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	struct pcmciamtd_dev *dev = (struct pcmciamtd_dev *)map->map_priv_1;
	unsigned long win_size = dev->win_size;

	DEBUG(3, "to = %p from = %lu len = %u", to, from, len);
	while(len) {
		int toread = win_size - (from & (win_size-1));
		caddr_t addr;

		if(toread > len)
			toread = len;
		
		addr = remap_window(map, from);
		if(!addr)
			return;

		DEBUG(4, "memcpy from %p to %p len = %d", addr, to, toread);
		memcpy_fromio(to, addr, toread);
		len -= toread;
		to += toread;
		from += toread;
	}
}


static void pcmcia_write8_remap(struct map_info *map, u8 d, unsigned long adr)
{
	caddr_t addr = remap_window(map, adr);

	if(!addr)
		return;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x", adr, addr, d);
	writeb(d, addr);
}


static void pcmcia_write16_remap(struct map_info *map, u16 d, unsigned long adr)
{
	caddr_t addr = remap_window(map, adr);
	if(!addr)
		return;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x", adr, addr, d);
	writew(d, addr);
}


static void pcmcia_copy_to_remap(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	struct pcmciamtd_dev *dev = (struct pcmciamtd_dev *)map->map_priv_1;
	unsigned long win_size = dev->win_size;

	DEBUG(3, "to = %lu from = %p len = %u", to, from, len);
	while(len) {
		int towrite = win_size - (to & (win_size-1));
		caddr_t addr;

		if(towrite > len)
			towrite = len;

		addr = remap_window(map, to);
		if(!addr)
			return;

		DEBUG(4, "memcpy from %p to %p len = %d", from, addr, towrite);
		memcpy_toio(addr, from, towrite);
		len -= towrite;
		to += towrite;
		from += towrite;
	}
}


/* read/write{8,16} copy_{from,to} routines with direct access */

static u8 pcmcia_read8(struct map_info *map, unsigned long ofs)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;
	u8 d;

	d = readb(win_base + ofs);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x", ofs, win_base + ofs, d);
	return d;
}


static u16 pcmcia_read16(struct map_info *map, unsigned long ofs)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;
	u16 d;

	d = readw(win_base + ofs);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x", ofs, win_base + ofs, d);
	return d;
}


static void pcmcia_copy_from(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;

	DEBUG(3, "to = %p from = %lu len = %u", to, from, len);
	memcpy_fromio(to, win_base + from, len);
}


static void pcmcia_write8(struct map_info *map, u8 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x", adr, win_base + adr, d);
	writeb(d, win_base + adr);
}


static void pcmcia_write16(struct map_info *map, u16 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x", adr, win_base + adr, d);
	writew(d, win_base + adr);
}


static void pcmcia_copy_to(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_2;

	DEBUG(3, "to = %lu from = %p len = %u", to, from, len);
	memcpy_toio(win_base + to, from, len);
}


static void pcmciamtd_set_vpp(struct map_info *map, int on)
{
	struct pcmciamtd_dev *dev = (struct pcmciamtd_dev *)map->map_priv_1;
	dev_link_t *link = &dev->link;
	modconf_t mod;
	int ret;

	mod.Attributes = CONF_VPP1_CHANGE_VALID | CONF_VPP2_CHANGE_VALID;
	mod.Vcc = 0;
	mod.Vpp1 = mod.Vpp2 = on ? dev->vpp : 0;

	DEBUG(2, "dev = %p on = %d vpp = %d\n", dev, on, dev->vpp);
	ret = CardServices(ModifyConfiguration, link->handle, &mod);
	if(ret != CS_SUCCESS) {
		cs_error(link->handle, ModifyConfiguration, ret);
	}
}


/* After a card is removed, pcmciamtd_release() will unregister the
 * device, and release the PCMCIA configuration.  If the device is
 * still open, this will be postponed until it is closed.
 */

static void pcmciamtd_release(u_long arg)
{
	dev_link_t *link = (dev_link_t *)arg;
	struct pcmciamtd_dev *dev = NULL;
	int ret;
	struct list_head *temp1, *temp2;

	DEBUG(3, "link = 0x%p", link);
	/* Find device in list */
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev = list_entry(temp1, struct pcmciamtd_dev, list);
		if(link == &dev->link)
			break;
	}
	if(link != &dev->link) {
		DEBUG(1, "Cant find %p in dev_list", link);
		return;
	}

	if(dev) {
		if(dev->mtd_info) {
			del_mtd_device(dev->mtd_info);
			dev->mtd_info = NULL;
			MOD_DEC_USE_COUNT;
		}
		if (link->win) {
			if(dev->win_base) {
				iounmap(dev->win_base);
				dev->win_base = NULL;
			}
			CardServices(ReleaseWindow, link->win);
		}
		ret = CardServices(ReleaseConfiguration, link->handle);
		if(ret != CS_SUCCESS)
			cs_error(link->handle, ReleaseConfiguration, ret);
			
	}
	link->state &= ~DEV_CONFIG;
}


static void card_settings(struct pcmciamtd_dev *dev, dev_link_t *link, int *new_name)
{
	int rc;
	tuple_t tuple;
	cisparse_t parse;
	u_char buf[64];

	tuple.Attributes = 0;
	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleDataMax = sizeof(buf);
	tuple.TupleOffset = 0;
	tuple.DesiredTuple = RETURN_FIRST_TUPLE;

	rc = CardServices(GetFirstTuple, link->handle, &tuple);
	while(rc == CS_SUCCESS) {
		rc = CardServices(GetTupleData, link->handle, &tuple);
		if(rc != CS_SUCCESS) {
			cs_error(link->handle, GetTupleData, rc);
			break;
		}
		rc = CardServices(ParseTuple, link->handle, &tuple, &parse);
		if(rc != CS_SUCCESS) {
			cs_error(link->handle, ParseTuple, rc);
			break;
		}
		
		switch(tuple.TupleCode) {
		case  CISTPL_FORMAT: {
			cistpl_format_t *t = &parse.format;
			(void)t; /* Shut up, gcc */
			DEBUG(2, "Format type: %u, Error Detection: %u, offset = %u, length =%u",
			      t->type, t->edc, t->offset, t->length);
			break;
			
		}
			
		case CISTPL_DEVICE: {
			cistpl_device_t *t = &parse.device;
			int i;
			DEBUG(2, "Common memory:");
			dev->pcmcia_map.size = t->dev[0].size;
			for(i = 0; i < t->ndev; i++) {
				DEBUG(2, "Region %d, type = %u", i, t->dev[i].type);
				DEBUG(2, "Region %d, wp = %u", i, t->dev[i].wp);
				DEBUG(2, "Region %d, speed = %u ns", i, t->dev[i].speed);
				DEBUG(2, "Region %d, size = %u bytes", i, t->dev[i].size);
			}
			break;
		}
			
		case CISTPL_VERS_1: {
			cistpl_vers_1_t *t = &parse.version_1;
			int i;
			if(t->ns) {
				dev->mtd_name[0] = '\0';
				for(i = 0; i < t->ns; i++) {
					if(i)
						strcat(dev->mtd_name, " ");
					strcat(dev->mtd_name, t->str+t->ofs[i]);
				}
			}
			DEBUG(2, "Found name: %s", dev->mtd_name);
			break;
		}
			
		case CISTPL_JEDEC_C: {
			cistpl_jedec_t *t = &parse.jedec;
			int i;
			for(i = 0; i < t->nid; i++) {
				DEBUG(2, "JEDEC: 0x%02x 0x%02x", t->id[i].mfr, t->id[i].info);
			}
			break;
		}
			
		case CISTPL_DEVICE_GEO: {
			cistpl_device_geo_t *t = &parse.device_geo;
			int i;
			dev->pcmcia_map.buswidth = t->geo[0].buswidth;
			for(i = 0; i < t->ngeo; i++) {
				DEBUG(2, "region: %d buswidth = %u", i, t->geo[i].buswidth);
				DEBUG(2, "region: %d erase_block = %u", i, t->geo[i].erase_block);
				DEBUG(2, "region: %d read_block = %u", i, t->geo[i].read_block);
				DEBUG(2, "region: %d write_block = %u", i, t->geo[i].write_block);
				DEBUG(2, "region: %d partition = %u", i, t->geo[i].partition);
				DEBUG(2, "region: %d interleave = %u", i, t->geo[i].interleave);
			}
			break;
		}
			
		default:
			DEBUG(2, "Unknown tuple code %d", tuple.TupleCode);
		}
		
		rc = CardServices(GetNextTuple, link->handle, &tuple, &parse);
	}
	if(!dev->pcmcia_map.size)
		dev->pcmcia_map.size = MAX_PCMCIA_ADDR;

	if(!dev->pcmcia_map.buswidth)
		dev->pcmcia_map.buswidth = 2;

	if(force_size) {
		dev->pcmcia_map.size = force_size << 20;
		DEBUG(2, "size forced to %dM", force_size);

	}

	if(buswidth) {
		dev->pcmcia_map.buswidth = buswidth;
		DEBUG(2, "buswidth forced to %d", buswidth);
	}		

	dev->pcmcia_map.name = dev->mtd_name;
	if(!dev->mtd_name[0]) {
		strcpy(dev->mtd_name, "PCMCIA Memory card");
		*new_name = 1;
	}

	DEBUG(1, "Device: Size: %lu Width:%d Name: %s",
	      dev->pcmcia_map.size, dev->pcmcia_map.buswidth << 3, dev->mtd_name);
}


/* pcmciamtd_config() is scheduled to run after a CARD_INSERTION event
 * is received, to configure the PCMCIA socket, and to make the
 * MTD device available to the system.
 */

#define CS_CHECK(fn, args...) \
while ((last_ret=CardServices(last_fn=(fn), args))!=0) goto cs_failed

static void pcmciamtd_config(dev_link_t *link)
{
	struct pcmciamtd_dev *dev = link->priv;
	struct mtd_info *mtd = NULL;
	cs_status_t status;
	win_req_t req;
	int last_ret = 0, last_fn = 0;
	int ret;
	int i;
	config_info_t t;
	static char *probes[] = { "jedec_probe", "cfi_probe" };
	cisinfo_t cisinfo;
	int new_name = 0;

	DEBUG(3, "link=0x%p", link);

	/* Configure card */
	link->state |= DEV_CONFIG;

	DEBUG(2, "Validating CIS");
	ret = CardServices(ValidateCIS, link->handle, &cisinfo);
	if(ret != CS_SUCCESS) {
		cs_error(link->handle, GetTupleData, ret);
	} else {
		DEBUG(2, "ValidateCIS found %d chains", cisinfo.Chains);
	}

	card_settings(dev, link, &new_name);

	dev->pcmcia_map.read8 = pcmcia_read8_remap;
	dev->pcmcia_map.read16 = pcmcia_read16_remap;
	dev->pcmcia_map.copy_from = pcmcia_copy_from_remap;
	dev->pcmcia_map.write8 = pcmcia_write8_remap;
	dev->pcmcia_map.write16 = pcmcia_write16_remap;
	dev->pcmcia_map.copy_to = pcmcia_copy_to_remap;
	if(setvpp == 1)
		dev->pcmcia_map.set_vpp = pcmciamtd_set_vpp;

	/* Request a memory window for PCMCIA. Some architeures can map windows upto the maximum
	   that PCMCIA can support (64Mb) - this is ideal and we aim for a window the size of the
	   whole card - otherwise we try smaller windows until we succeed */

	req.Attributes =  WIN_MEMORY_TYPE_CM | WIN_ENABLE;
	req.Attributes |= (dev->pcmcia_map.buswidth == 1) ? WIN_DATA_WIDTH_8 : WIN_DATA_WIDTH_16;
	req.Base = 0;
	req.AccessSpeed = mem_speed;
	link->win = (window_handle_t)link->handle;
	req.Size = (force_size) ? force_size << 20 : MAX_PCMCIA_ADDR;
	dev->win_size = 0;

	do {
		int ret;
		DEBUG(2, "requesting window with size = %dKB memspeed = %d",
		      req.Size >> 10, req.AccessSpeed);
		link->win = (window_handle_t)link->handle;
		ret = CardServices(RequestWindow, &link->win, &req);
		DEBUG(2, "ret = %d dev->win_size = %d", ret, dev->win_size);
		if(ret) {
			req.Size >>= 1;
		} else {
			DEBUG(2, "Got window of size %dKB", req.Size >> 10);
			dev->win_size = req.Size;
			break;
		}
	} while(req.Size >= 0x1000);

	DEBUG(2, "dev->win_size = %d", dev->win_size);

	if(!dev->win_size) {
		err("Cant allocate memory window");
		pcmciamtd_release((u_long)link);
		return;
	}
	DEBUG(1, "Allocated a window of %dKB", dev->win_size >> 10);
		
	/* Get write protect status */
	CS_CHECK(GetStatus, link->handle, &status);
	DEBUG(2, "status value: 0x%x window handle = 0x%8.8lx",
	      status.CardState, (unsigned long)link->win);
	dev->win_base = ioremap(req.Base, req.Size);
	if(!dev->win_base) {
		err("ioremap(%lu, %u) failed", req.Base, req.Size);
		pcmciamtd_release((u_long)link);
		return;
	}
	DEBUG(1, "mapped window dev = %p req.base = 0x%lx base = %p size = 0x%x",
	      dev, req.Base, dev->win_base, req.Size);
	dev->cardsize = 0;
	dev->offset = 0;

	dev->pcmcia_map.map_priv_1 = (unsigned long)dev;
	dev->pcmcia_map.map_priv_2 = (unsigned long)link->win;

	DEBUG(2, "Getting configuration");
	CS_CHECK(GetConfigurationInfo, link->handle, &t);
	DEBUG(2, "Vcc = %d Vpp1 = %d Vpp2 = %d", t.Vcc, t.Vpp1, t.Vpp2);
	dev->vpp = (vpp) ? vpp : t.Vpp1;
	link->conf.Attributes = 0;
	link->conf.Vcc = t.Vcc;
	if(setvpp == 2) {
		link->conf.Vpp1 = dev->vpp;
		link->conf.Vpp2 = dev->vpp;
	} else {
		link->conf.Vpp1 = 0;
		link->conf.Vpp2 = 0;
	}

	link->conf.IntType = INT_MEMORY;
	link->conf.ConfigBase = t.ConfigBase;
	link->conf.Status = t.Status;
	link->conf.Pin = t.Pin;
	link->conf.Copy = t.Copy;
	link->conf.ExtStatus = t.ExtStatus;
	link->conf.ConfigIndex = 0;
	link->conf.Present = t.Present;
	DEBUG(2, "Setting Configuration");
	ret = CardServices(RequestConfiguration, link->handle, &link->conf);
	if(ret != CS_SUCCESS) {
		cs_error(link->handle, RequestConfiguration, ret);
	}

	link->dev = NULL;
	link->state &= ~DEV_CONFIG_PENDING;

	if(mem_type == 1) {
		mtd = do_map_probe("map_ram", &dev->pcmcia_map);
	} else if(mem_type == 2) {
		mtd = do_map_probe("map_rom", &dev->pcmcia_map);
	} else {
		for(i = 0; i < sizeof(probes) / sizeof(char *); i++) {
			DEBUG(1, "Trying %s", probes[i]);
			mtd = do_map_probe(probes[i], &dev->pcmcia_map);
			if(mtd)
				break;
			
			DEBUG(1, "FAILED: %s", probes[i]);
		}
	}
	
	if(!mtd) {
		DEBUG(1, "Cant find an MTD");
		pcmciamtd_release((u_long)link);
		return;
	}

	dev->mtd_info = mtd;
	mtd->module = THIS_MODULE;
	dev->cardsize = mtd->size;

	if(new_name) {
		int size = 0;
		char unit = ' ';
		/* Since we are using a default name, make it better by adding in the
		   size */
		if(mtd->size < 1048576) { /* <1MB in size, show size in K */
			size = mtd->size >> 10;
			unit = 'K'; 
		} else {
			size = mtd->size >> 20;
			unit = 'M';
		}
		sprintf(mtd->name, "%d%cB %s", size, unit, "PCMCIA Memory card");
	}

	/* If the memory found is fits completely into the mapped PCMCIA window,
	   use the faster non-remapping read/write functions */
	if(dev->cardsize <= dev->win_size) {
		DEBUG(1, "Using non remapping memory functions");

		dev->pcmcia_map.map_priv_2 = (unsigned long)dev->win_base;
		dev->pcmcia_map.read8 = pcmcia_read8;
		dev->pcmcia_map.read16 = pcmcia_read16;
		dev->pcmcia_map.copy_from = pcmcia_copy_from;
		dev->pcmcia_map.write8 = pcmcia_write8;
		dev->pcmcia_map.write16 = pcmcia_write16;
		dev->pcmcia_map.copy_to = pcmcia_copy_to;
	}

	MOD_INC_USE_COUNT;
	if(add_mtd_device(mtd)) {
		dev->mtd_info = NULL;
		MOD_DEC_USE_COUNT;
		err("Couldnt register MTD device");
		pcmciamtd_release((u_long)link);
		return;
	}
	DEBUG(1, "mtd added @ %p mtd->priv = %p", mtd, mtd->priv);

	return;

 cs_failed:
	cs_error(link->handle, last_fn, last_ret);
	err("CS Error, exiting");
	pcmciamtd_release((u_long)link);
	return;
}


/* The card status event handler.  Mostly, this schedules other
 * stuff to run after an event is received.  A CARD_REMOVAL event
 * also sets some flags to discourage the driver from trying
 * to talk to the card any more.
 */

static int pcmciamtd_event(event_t event, int priority,
			event_callback_args_t *args)
{
	dev_link_t *link = args->client_data;

	DEBUG(1, "event=0x%06x", event);
	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		DEBUG(2, "EVENT_CARD_REMOVAL");
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG)
			mod_timer(&link->release, jiffies + HZ/20);
		break;
	case CS_EVENT_CARD_INSERTION:
		DEBUG(2, "EVENT_CARD_INSERTION");
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		pcmciamtd_config(link);
		break;
	case CS_EVENT_PM_SUSPEND:
		DEBUG(2, "EVENT_PM_SUSPEND");
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		DEBUG(2, "EVENT_RESET_PHYSICAL");
		/* get_lock(link); */
		break;
	case CS_EVENT_PM_RESUME:
		DEBUG(2, "EVENT_PM_RESUME");
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		DEBUG(2, "EVENT_CARD_RESET");
		/* free_lock(link); */
		break;
	default:
		DEBUG(2, "Unknown event %d", event);
	}
	return 0;
}


/* This deletes a driver "instance".  The device is de-registered
 * with Card Services.  If it has been released, all local data
 * structures are freed.  Otherwise, the structures will be freed
 * when the device is released.
 */

static void pcmciamtd_detach(dev_link_t *link)
{
	int ret;
	struct pcmciamtd_dev *dev = NULL;
	struct list_head *temp1, *temp2;

	DEBUG(3, "link=0x%p", link);

	/* Find device in list */
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev = list_entry(temp1, struct pcmciamtd_dev, list);
		if(link == &dev->link)
			break;
	}
	if(link != &dev->link) {
		DEBUG(1, "Cant find %p in dev_list", link);
		return;
	}
	
	del_timer(&link->release);

	if(!dev) {
		DEBUG(3, "dev is NULL");
		return;
	}

	if (link->state & DEV_CONFIG) {
		//pcmciamtd_release((u_long)link);
		DEBUG(3, "DEV_CONFIG set");
		link->state |= DEV_STALE_LINK;
		return;
	}

	if (link->handle) {
		DEBUG(2, "Deregistering with card services");
		ret = CardServices(DeregisterClient, link->handle);
		if (ret != CS_SUCCESS)
			cs_error(link->handle, DeregisterClient, ret);
	}
	DEBUG(3, "Freeing dev (%p)", dev);
	list_del(&dev->list);
	link->priv = NULL;
	kfree(dev);
}


/* pcmciamtd_attach() creates an "instance" of the driver, allocating
 * local data structures for one device.  The device is registered
 * with Card Services.
 */

static dev_link_t *pcmciamtd_attach(void)
{
	struct pcmciamtd_dev *dev;
	dev_link_t *link;
	client_reg_t client_reg;
	int ret;

	/* Create new memory card device */
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) return NULL;
	DEBUG(1, "dev=0x%p", dev);

	memset(dev, 0, sizeof(*dev));
	link = &dev->link; link->priv = dev;

	link->release.function = &pcmciamtd_release;
	link->release.data = (u_long)link;

	link->conf.Attributes = 0;
	link->conf.IntType = INT_MEMORY;

	list_add(&dev->list, &dev_list);

	/* Register with Card Services */
	client_reg.dev_info = &dev_info;
	client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
	client_reg.EventMask =
		CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
		CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
		CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
	client_reg.event_handler = &pcmciamtd_event;
	client_reg.Version = 0x0210;
	client_reg.event_callback_args.client_data = link;
	DEBUG(2, "Calling RegisterClient");
	ret = CardServices(RegisterClient, &link->handle, &client_reg);
	if (ret != 0) {
		cs_error(link->handle, RegisterClient, ret);
		pcmciamtd_detach(link);
		return NULL;
	}

	return link;
}


static int __init init_pcmciamtd(void)
{
	servinfo_t serv;

	info(DRIVER_DESC " " DRIVER_VERSION);
	CardServices(GetCardServicesInfo, &serv);
	if (serv.Revision != CS_RELEASE_CODE) {
		err("Card Services release does not match!");
		return -1;
	}

	if(buswidth && buswidth != 1 && buswidth != 2) {
		info("bad buswidth (%d), using default", buswidth);
		buswidth = 2;
	}
	if(force_size && (force_size < 1 || force_size > 64)) {
		info("bad force_size (%d), using default", force_size);
		force_size = 0;
	}
	if(mem_type && mem_type != 1 && mem_type != 2) {
		info("bad mem_type (%d), using default", mem_type);
		mem_type = 0;
	}
	register_pccard_driver(&dev_info, &pcmciamtd_attach, &pcmciamtd_detach);
	return 0;
}


static void __exit exit_pcmciamtd(void)
{
	struct list_head *temp1, *temp2;

	DEBUG(1, DRIVER_DESC " unloading");
	unregister_pccard_driver(&dev_info);
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev_link_t *link = &list_entry(temp1, struct pcmciamtd_dev, list)->link;
		if (link && (link->state & DEV_CONFIG)) {
			pcmciamtd_release((u_long)link);
			pcmciamtd_detach(link);
		}
	}
}

module_init(init_pcmciamtd);
module_exit(exit_pcmciamtd);
