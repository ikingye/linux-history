/*
 *  AMD K7 Powernow driver.
 *  (C) 2003 Dave Jones <davej@codemonkey.org.uk> on behalf of SuSE Labs.
 *  (C) 2003-2004 Dave Jones <davej@redhat.com>
 *
 *  Licensed under the terms of the GNU GPL License version 2.
 *  Based upon datasheets & sample CPUs kindly provided by AMD.
 *
 * Errata 5: Processor may fail to execute a FID/VID change in presence of interrupt.
 * - We cli/sti on stepping A0 CPUs around the FID/VID transition.
 * Errata 15: Processors with half frequency multipliers may hang upon wakeup from disconnect.
 * - We disable half multipliers if ACPI is used on A0 stepping CPUs.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dmi.h>

#include <asm/msr.h>
#include <asm/timex.h>
#include <asm/io.h>
#include <asm/system.h>

#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
#include <linux/acpi.h>
#include <acpi/processor.h>
#endif

#include "powernow-k7.h"

#define PFX "powernow: "


struct psb_s {
	u8 signature[10];
	u8 tableversion;
	u8 flags;
	u16 settlingtime;
	u8 reserved1;
	u8 numpst;
};

struct pst_s {
	u32 cpuid;
	u8 fsbspeed;
	u8 maxfid;
	u8 startvid;
	u8 numpstates;
};

#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
union powernow_acpi_control_t {
	struct {
		unsigned long fid:5,
		vid:5,
		sgtc:20,
		res1:2;
	} bits;
	unsigned long val;
};
#endif

/* divide by 1000 to get VID. */
static int mobile_vid_table[32] = {
    2000, 1950, 1900, 1850, 1800, 1750, 1700, 1650,
    1600, 1550, 1500, 1450, 1400, 1350, 1300, 0,
    1275, 1250, 1225, 1200, 1175, 1150, 1125, 1100,
    1075, 1050, 1024, 1000, 975, 950, 925, 0,
};

/* divide by 10 to get FID. */
static int fid_codes[32] = {
    110, 115, 120, 125, 50, 55, 60, 65,
    70, 75, 80, 85, 90, 95, 100, 105,
    30, 190, 40, 200, 130, 135, 140, 210,
    150, 225, 160, 165, 170, 180, -1, -1,
};

/* This parameter is used in order to force ACPI instead of legacy method for
 * configuration purpose.
 */

static int acpi_force;
static int debug;

static struct cpufreq_frequency_table *powernow_table;

static unsigned int can_scale_bus;
static unsigned int can_scale_vid;
static unsigned int minimum_speed=-1;
static unsigned int maximum_speed;
static unsigned int number_scales;
static unsigned int fsb;
static unsigned int latency;
static char have_a0;

static void dprintk(const char *fmt, ...)
{
	char s[256];
	va_list args;

	if (debug==0)
		return;

	va_start(args,fmt);
	vsprintf(s, fmt, args);
	printk(s);
	va_end(args);
}


static int check_fsb(unsigned int fsbspeed)
{
	int delta;
	unsigned int f = fsb / 1000;

	delta = (fsbspeed > f) ? fsbspeed - f : f - fsbspeed;
	return (delta < 5);
}

static int check_powernow(void)
{
	struct cpuinfo_x86 *c = cpu_data;
	unsigned int maxei, eax, ebx, ecx, edx;

	if ((c->x86_vendor != X86_VENDOR_AMD) || (c->x86 !=6)) {
#ifdef MODULE
		printk (KERN_INFO PFX "This module only works with AMD K7 CPUs\n");
#endif
		return 0;
	}

	/* Get maximum capabilities */
	maxei = cpuid_eax (0x80000000);
	if (maxei < 0x80000007) {	/* Any powernow info ? */
#ifdef MODULE
		printk (KERN_INFO PFX "No powernow capabilities detected\n");
#endif
		return 0;
	}

	if ((c->x86_model == 6) && (c->x86_mask == 0)) {
		printk (KERN_INFO PFX "K7 660[A0] core detected, enabling errata workarounds\n");
		have_a0 = 1;
	}

	cpuid(0x80000007, &eax, &ebx, &ecx, &edx);

	/* Check we can actually do something before we say anything.*/
	if (!(edx & (1 << 1 | 1 << 2)))
		return 0;

	printk (KERN_INFO PFX "PowerNOW! Technology present. Can scale: ");

	if (edx & 1 << 1) {
		printk ("frequency");
		can_scale_bus=1;
	}

	if ((edx & (1 << 1 | 1 << 2)) == 0x6)
		printk (" and ");

	if (edx & 1 << 2) {
		printk ("voltage");
		can_scale_vid=1;
	}

	printk (".\n");
	return 1;
}


static int get_ranges (unsigned char *pst)
{
	unsigned int j;
	unsigned int speed;
	u8 fid, vid;

	powernow_table = kmalloc((sizeof(struct cpufreq_frequency_table) * (number_scales + 1)), GFP_KERNEL);
	if (!powernow_table)
		return -ENOMEM;
	memset(powernow_table, 0, (sizeof(struct cpufreq_frequency_table) * (number_scales + 1)));

	for (j=0 ; j < number_scales; j++) {
		fid = *pst++;

		powernow_table[j].frequency = (fsb * fid_codes[fid]) / 10;
		powernow_table[j].index = fid; /* lower 8 bits */

		speed = powernow_table[j].frequency;

		if ((fid_codes[fid] % 10)==5) {
#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
			if (have_a0 == 1)
				powernow_table[j].frequency = CPUFREQ_ENTRY_INVALID;
#endif
		}

		dprintk (KERN_INFO PFX "   FID: 0x%x (%d.%dx [%dMHz])  ", fid,
			fid_codes[fid] / 10, fid_codes[fid] % 10, speed/1000);

		if (speed < minimum_speed)
			minimum_speed = speed;
		if (speed > maximum_speed)
			maximum_speed = speed;

		vid = *pst++;
		powernow_table[j].index |= (vid << 8); /* upper 8 bits */
		dprintk ("VID: 0x%x (%d.%03dV)\n", vid,	mobile_vid_table[vid]/1000,
			mobile_vid_table[vid]%1000);
	}
	powernow_table[number_scales].frequency = CPUFREQ_TABLE_END;
	powernow_table[number_scales].index = 0;

	return 0;
}


static void change_FID(int fid)
{
	union msr_fidvidctl fidvidctl;

	rdmsrl (MSR_K7_FID_VID_CTL, fidvidctl.val);
	if (fidvidctl.bits.FID != fid) {
		fidvidctl.bits.SGTC = latency;
		fidvidctl.bits.FID = fid;
		fidvidctl.bits.VIDC = 0;
		fidvidctl.bits.FIDC = 1;
		wrmsrl (MSR_K7_FID_VID_CTL, fidvidctl.val);
	}
}


static void change_VID(int vid)
{
	union msr_fidvidctl fidvidctl;

	rdmsrl (MSR_K7_FID_VID_CTL, fidvidctl.val);
	if (fidvidctl.bits.VID != vid) {
		fidvidctl.bits.SGTC = latency;
		fidvidctl.bits.VID = vid;
		fidvidctl.bits.FIDC = 0;
		fidvidctl.bits.VIDC = 1;
		wrmsrl (MSR_K7_FID_VID_CTL, fidvidctl.val);
	}
}


static void change_speed (unsigned int index)
{
	u8 fid, vid;
	struct cpufreq_freqs freqs;
	union msr_fidvidstatus fidvidstatus;
	int cfid;

	/* fid are the lower 8 bits of the index we stored into
	 * the cpufreq frequency table in powernow_decode_bios,
	 * vid are the upper 8 bits.
	 */

	fid = powernow_table[index].index & 0xFF;
	vid = (powernow_table[index].index & 0xFF00) >> 8;

	freqs.cpu = 0;

	rdmsrl (MSR_K7_FID_VID_STATUS, fidvidstatus.val);
	cfid = fidvidstatus.bits.CFID;
	freqs.old = fsb * fid_codes[cfid] / 10;

	freqs.new = powernow_table[index].frequency;

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

	/* Now do the magic poking into the MSRs.  */

	if (have_a0 == 1)	/* A0 errata 5 */
		local_irq_disable();

	if (freqs.old > freqs.new) {
		/* Going down, so change FID first */
		change_FID(fid);
		change_VID(vid);
	} else {
		/* Going up, so change VID first */
		change_VID(vid);
		change_FID(fid);
	}


	if (have_a0 == 1)
		local_irq_enable();

	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
}


#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)

struct acpi_processor_performance *acpi_processor_perf;

static int powernow_acpi_init(void)
{
	int i;
	int retval = 0;
	union powernow_acpi_control_t pc;

	if (acpi_processor_perf != NULL && powernow_table != NULL) {
		retval = -EINVAL;
		goto err0;
	}

	acpi_processor_perf = kmalloc(sizeof(struct acpi_processor_performance),
				      GFP_KERNEL);

	if (!acpi_processor_perf) {
		retval = -ENOMEM;
		goto err0;
	}

	memset(acpi_processor_perf, 0, sizeof(struct acpi_processor_performance));

	if (acpi_processor_register_performance(acpi_processor_perf, 0)) {
		retval = -EIO;
		goto err1;
	}

	if (acpi_processor_perf->control_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE) {
		retval = -ENODEV;
		goto err2;
	}

	if (acpi_processor_perf->status_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE) {
		retval = -ENODEV;
		goto err2;
	}

	number_scales = acpi_processor_perf->state_count;

	if (number_scales < 2) {
		retval = -ENODEV;
		goto err2;
	}

	powernow_table = kmalloc((number_scales + 1) * (sizeof(struct cpufreq_frequency_table)), GFP_KERNEL);
	if (!powernow_table) {
		retval = -ENOMEM;
		goto err2;
	}

	memset(powernow_table, 0, ((number_scales + 1) * sizeof(struct cpufreq_frequency_table)));

	pc.val = (unsigned long) acpi_processor_perf->states[0].control;
	for (i = 0; i < number_scales; i++) {
		u8 fid, vid;
		unsigned int speed;

		pc.val = (unsigned long) acpi_processor_perf->states[i].control;
		dprintk (KERN_INFO PFX "acpi:  P%d: %d MHz %d mW %d uS control %08x SGTC %d\n",
			 i,
			 (u32) acpi_processor_perf->states[i].core_frequency,
			 (u32) acpi_processor_perf->states[i].power,
			 (u32) acpi_processor_perf->states[i].transition_latency,
			 (u32) acpi_processor_perf->states[i].control,
			 pc.bits.sgtc);

		vid = pc.bits.vid;
		fid = pc.bits.fid;

		powernow_table[i].frequency = fsb * fid_codes[fid] / 10;
		powernow_table[i].index = fid; /* lower 8 bits */
		powernow_table[i].index |= (vid << 8); /* upper 8 bits */

		speed = powernow_table[i].frequency;

		if ((fid_codes[fid] % 10)==5) {
			if (have_a0 == 1)
				powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
		}

		dprintk (KERN_INFO PFX "   FID: 0x%x (%d.%dx [%dMHz])  ", fid,
			fid_codes[fid] / 10, fid_codes[fid] % 10, speed/1000);
		dprintk ("VID: 0x%x (%d.%03dV)\n", vid,	mobile_vid_table[vid]/1000,
			mobile_vid_table[vid]%1000);

		if (latency < pc.bits.sgtc)
			latency = pc.bits.sgtc;

		if (speed < minimum_speed)
			minimum_speed = speed;
		if (speed > maximum_speed)
			maximum_speed = speed;
	}

	powernow_table[i].frequency = CPUFREQ_TABLE_END;
	powernow_table[i].index = 0;

	return 0;

err2:
	acpi_processor_unregister_performance(acpi_processor_perf, 0);
err1:
	kfree(acpi_processor_perf);
err0:
	printk(KERN_WARNING PFX "ACPI perflib can not be used in this platform\n");
	acpi_processor_perf = NULL;
	return retval;
}
#else
static int powernow_acpi_init(void)
{
	printk(KERN_INFO PFX "no support for ACPI processor found."
	       "  Please recompile your kernel with ACPI processor\n");
	return -EINVAL;
}
#endif

static int powernow_decode_bios (int maxfid, int startvid)
{
	struct psb_s *psb;
	struct pst_s *pst;
	unsigned int i, j;
	unsigned char *p;
	unsigned int etuple;
	unsigned int ret;

	etuple = cpuid_eax(0x80000001);

	for (i=0xC0000; i < 0xffff0 ; i+=16) {

		p = phys_to_virt(i);

		if (memcmp(p, "AMDK7PNOW!",  10) == 0){
			dprintk (KERN_INFO PFX "Found PSB header at %p\n", p);
			psb = (struct psb_s *) p;
			dprintk (KERN_INFO PFX "Table version: 0x%x\n", psb->tableversion);
			if (psb->tableversion != 0x12) {
				printk (KERN_INFO PFX "Sorry, only v1.2 tables supported right now\n");
				return -ENODEV;
			}

			dprintk (KERN_INFO PFX "Flags: 0x%x (", psb->flags);
			if ((psb->flags & 1)==0) {
				dprintk ("Mobile");
			} else {
				dprintk ("Desktop");
			}
			dprintk (" voltage regulator)\n");

			latency = psb->settlingtime;
			if (latency < 100) {
				printk (KERN_INFO PFX "BIOS set settling time to %d microseconds."
						"Should be at least 100. Correcting.\n", latency);
				latency = 100;
			}
			dprintk (KERN_INFO PFX "Settling Time: %d microseconds.\n", psb->settlingtime);
			dprintk (KERN_INFO PFX "Has %d PST tables. (Only dumping ones relevant to this CPU).\n", psb->numpst);

			p += sizeof (struct psb_s);

			pst = (struct pst_s *) p;

			for (i = 0 ; i <psb->numpst; i++) {
				pst = (struct pst_s *) p;
				number_scales = pst->numpstates;

				if ((etuple == pst->cpuid) && check_fsb(pst->fsbspeed) &&
				    (maxfid==pst->maxfid) && (startvid==pst->startvid))
				{
					dprintk (KERN_INFO PFX "PST:%d (@%p)\n", i, pst);
					dprintk (KERN_INFO PFX " cpuid: 0x%x  ", pst->cpuid);
					dprintk ("fsb: %d  ", pst->fsbspeed);
					dprintk ("maxFID: 0x%x  ", pst->maxfid);
					dprintk ("startvid: 0x%x\n", pst->startvid);

					ret = get_ranges ((char *) pst + sizeof (struct pst_s));
					return ret;

				} else {
					p = (char *) pst + sizeof (struct pst_s);
					for (j=0 ; j < number_scales; j++)
						p+=2;
				}
			}
			printk (KERN_INFO PFX "No PST tables match this cpuid (0x%x)\n", etuple);
			printk (KERN_INFO PFX "This is indicative of a broken BIOS.\n");

			return -EINVAL;
		}
		p++;
	}

	return -ENODEV;
}


static int powernow_target (struct cpufreq_policy *policy,
			    unsigned int target_freq,
			    unsigned int relation)
{
	unsigned int newstate;

	if (cpufreq_frequency_table_target(policy, powernow_table, target_freq, relation, &newstate))
		return -EINVAL;

	change_speed(newstate);

	return 0;
}


static int powernow_verify (struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, powernow_table);
}

/*
 * We use the fact that the bus frequency is somehow
 * a multiple of 100000/3 khz, then we compute sgtc according
 * to this multiple.
 * That way, we match more how AMD thinks all of that work.
 * We will then get the same kind of behaviour already tested under
 * the "well-known" other OS.
 */
static int __init fixup_sgtc(void)
{
	unsigned int sgtc;
	unsigned int m;

	m = fsb / 3333;
	if ((m % 10) >= 5)
		m += 5;

	m /= 10;

	sgtc = 100 * m * latency;
	sgtc = sgtc / 3;
	if (sgtc > 0xfffff) {
		printk(KERN_WARNING PFX "SGTC too large %d\n", sgtc);
		sgtc = 0xfffff;
	}
	return sgtc;
}

static unsigned int powernow_get(unsigned int cpu)
{
	union msr_fidvidstatus fidvidstatus;
	unsigned int cfid;

	if (cpu)
		return 0;
	rdmsrl (MSR_K7_FID_VID_STATUS, fidvidstatus.val);
	cfid = fidvidstatus.bits.CFID;

	return (fsb * fid_codes[cfid] / 10);
}


static int __init acer_cpufreq_pst(struct dmi_system_id *d)
{
	printk(KERN_WARNING "%s laptop with broken PST tables in BIOS detected.\n", d->ident);
	printk(KERN_WARNING "You need to downgrade to 3A21 (09/09/2002), or try a newer BIOS than 3A71 (01/20/2003)\n");
	printk(KERN_WARNING "cpufreq scaling has been disabled as a result of this.\n");
	return 0;
}

/*
 * Some Athlon laptops have really fucked PST tables.
 * A BIOS update is all that can save them.
 * Mention this, and disable cpufreq.
 */
static struct dmi_system_id __initdata powernow_dmi_table[] = {
	{
		.callback = acer_cpufreq_pst,
		.ident = "Acer Aspire",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde Software"),
			DMI_MATCH(DMI_BIOS_VERSION, "3A71"),
		},
	},
	{ }
};

static int __init powernow_cpu_init (struct cpufreq_policy *policy)
{
	union msr_fidvidstatus fidvidstatus;
	int result;

	if (policy->cpu != 0)
		return -ENODEV;

	rdmsrl (MSR_K7_FID_VID_STATUS, fidvidstatus.val);

	/* A K7 with powernow technology is set to max frequency by BIOS */
	fsb = (10 * cpu_khz) / fid_codes[fidvidstatus.bits.MFID];
	if (!fsb) {
		printk(KERN_WARNING PFX "can not determine bus frequency\n");
		return -EINVAL;
	}
	dprintk(KERN_INFO PFX "FSB: %3d.%03d MHz\n", fsb/1000, fsb%1000);

 	if (dmi_check_system(powernow_dmi_table) || acpi_force) {
		printk (KERN_INFO PFX "PSB/PST known to be broken.  Trying ACPI instead\n");
		result = powernow_acpi_init();
	} else {
		result = powernow_decode_bios(fidvidstatus.bits.MFID, fidvidstatus.bits.SVID);
		if (result) {
			printk (KERN_INFO PFX "Trying ACPI perflib\n");
			maximum_speed = 0;
			minimum_speed = -1;
			latency = 0;
			result = powernow_acpi_init();
			if (result) {
				printk (KERN_INFO PFX "ACPI and legacy methods failed\n");
				printk (KERN_INFO PFX "See http://www.codemonkey.org.uk/projects/cpufreq/powernow-k7.shtml\n");
			}
		} else {
			/* SGTC use the bus clock as timer */
			latency = fixup_sgtc();
			printk(KERN_INFO PFX "SGTC: %d\n", latency);
		}
	}

	if (result)
		return result;

	printk (KERN_INFO PFX "Minimum speed %d MHz. Maximum speed %d MHz.\n",
				minimum_speed/1000, maximum_speed/1000);

	policy->governor = CPUFREQ_DEFAULT_GOVERNOR;

	policy->cpuinfo.transition_latency = 20 * latency / fsb;

	policy->cur = powernow_get(0);

	cpufreq_frequency_table_get_attr(powernow_table, policy->cpu);

	return cpufreq_frequency_table_cpuinfo(policy, powernow_table);
}

static int powernow_cpu_exit (struct cpufreq_policy *policy) {
	cpufreq_frequency_table_put_attr(policy->cpu);
	return 0;
}

static struct freq_attr* powernow_table_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver powernow_driver = {
	.verify	= powernow_verify,
	.target	= powernow_target,
	.get	= powernow_get,
	.init	= powernow_cpu_init,
	.exit	= powernow_cpu_exit,
	.name	= "powernow-k7",
	.owner	= THIS_MODULE,
	.attr	= powernow_table_attr,
};

static int __init powernow_init (void)
{
	if (check_powernow()==0)
		return -ENODEV;
	return cpufreq_register_driver(&powernow_driver);
}


static void __exit powernow_exit (void)
{
#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
	if (acpi_processor_perf) {
		acpi_processor_unregister_performance(acpi_processor_perf, 0);
		kfree(acpi_processor_perf);
	}
#endif
	cpufreq_unregister_driver(&powernow_driver);
	if (powernow_table)
		kfree(powernow_table);
}

module_param(debug, int, 0444);
MODULE_PARM_DESC(debug, "enable debug output.");
module_param(acpi_force,  int, 0444);
MODULE_PARM_DESC(acpi_force, "Force ACPI to be used.");

MODULE_AUTHOR ("Dave Jones <davej@codemonkey.org.uk>");
MODULE_DESCRIPTION ("Powernow driver for AMD K7 processors.");
MODULE_LICENSE ("GPL");

late_initcall(powernow_init);
module_exit(powernow_exit);

