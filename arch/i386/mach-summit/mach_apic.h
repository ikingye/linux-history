#ifndef __ASM_MACH_APIC_H
#define __ASM_MACH_APIC_H

extern int x86_summit;

#define XAPIC_DEST_CPUS_MASK    0x0Fu
#define XAPIC_DEST_CLUSTER_MASK 0xF0u

#define xapic_phys_to_log_apicid(phys_apic) ( (1ul << ((phys_apic) & 0x3)) |\
		((phys_apic) & XAPIC_DEST_CLUSTER_MASK) )

static inline unsigned long calculate_ldr(unsigned long old)
{
	unsigned long id;

	if (x86_summit)
		id = xapic_phys_to_log_apicid(hard_smp_processor_id());
	else
		id = 1UL << smp_processor_id();
	return ((old & ~APIC_LDR_MASK) | SET_APIC_LOGICAL_ID(id));
}

#define APIC_DFR_VALUE	(x86_summit ? APIC_DFR_CLUSTER : APIC_DFR_FLAT)
#define TARGET_CPUS	(x86_summit ? XAPIC_DEST_CPUS_MASK : cpu_online_map)

#define APIC_BROADCAST_ID     (x86_summit ? 0xFF : 0x0F)
#define check_apicid_used(bitmap, apicid) (0)

static inline void summit_check(char *oem, char *productid)
{
	if (!strncmp(oem, "IBM ENSW", 8) && !strncmp(str, "VIGIL SMP", 9))
		x86_summit = 1;
}

static inline void clustered_apic_check(void)
{
	printk("Enabling APIC mode:  %s.  Using %d I/O APICs\n",
		(x86_summit ? "Summit" : "Flat"), nr_ioapics);
}

#endif /* __ASM_MACH_APIC_H */
