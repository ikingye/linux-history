/*
 * These are the public elements of the Linux kernel NET/ROM implementation.
 * For kernel AX.25 see the file ax25.h. This file requires ax25.h for the
 * definition of the ax25_address structure.
 */
 
#ifndef	NETROM_KERNEL_H
#define	NETROM_KERNEL_H

#define PF_NETROM	AF_NETROM
#define NETROM_MTU	236

#define NETROM_T1	1
#define NETROM_T2	2
#define NETROM_N2	3
#define	NETROM_HDRINCL	4
#define	NETROM_PACLEN	5
#define	NETROM_T4	6
#define	NETROM_IDLE	7

#define	NETROM_KILL	99

#define	SIOCNRDECOBS		(SIOCPROTOPRIVATE+0)
#define	SIOCNRCTLCON		(SIOCPROTOPRIVATE+1)

struct nr_route_struct {
#define	NETROM_NEIGH	0
#define	NETROM_NODE	1
	int type;
	ax25_address callsign;
	char device[16];
	unsigned int quality;
	char mnemonic[7];
	ax25_address neighbour;
	unsigned int obs_count;
	unsigned int ndigis;
	ax25_address digipeaters[AX25_MAX_DIGIS];
};

struct nr_ctl_struct {
	unsigned char index;
	unsigned char id;
	unsigned int  cmd;
	unsigned long arg;
};

#endif
