/* $Id: init.c,v 1.7 1997/03/24 17:43:59 jj Exp $
 * init.c:  Initialize internal variables used by the PROM
 *          library functions.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 * Copyright (C) 1996,1997 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>

#include <asm/openprom.h>
#include <asm/oplib.h>

enum prom_major_version prom_vers;
unsigned int prom_rev, prom_prev;

/* The root node of the prom device tree. */
int prom_root_node;
int prom_stdin, prom_stdout;
int prom_chosen_node;

/* You must call prom_init() before you attempt to use any of the
 * routines in the prom library.  It returns 0 on success, 1 on
 * failure.  It gets passed the pointer to the PROM vector.
 */

extern void prom_meminit(void);
extern void prom_ranges_init(void);
extern void prom_cif_init(void *, void *);

__initfunc(void prom_init(void *cif_handler, void *cif_stack))
{
	char buffer[80];
	int node;
	
	prom_vers = PROM_P1275;
	
	prom_cif_init(cif_handler, cif_stack);

	prom_root_node = prom_getsibling(0);
	if((prom_root_node == 0) || (prom_root_node == -1))
		prom_halt();

	prom_chosen_node = prom_finddevice("/chosen");
	if (!prom_chosen_node || prom_chosen_node == -1)
		prom_halt();
		
	prom_stdin = prom_getint (prom_chosen_node, "stdin");
	prom_stdout = prom_getint (prom_chosen_node, "stdout");

	node = prom_finddevice("/openprom");
	if (!node || node == -1)
		prom_halt();
		
	prom_getstring (node, "version", buffer, sizeof (buffer));
	
	prom_printf ("\n");
	
	if (strncmp (buffer, "OBP ", 4) || buffer[5] != '.' || buffer[7] != '.') {
		prom_printf ("Strange OBP version `%s'.\n", buffer);
		prom_halt ();
	}
	/* Version field is expected to be 'OBP x.y.z date...' */
	
	prom_rev = buffer[6] - '0';
	prom_prev = ((buffer[4] - '0') << 16) | 
		    ((buffer[6] - '0') << 8) |
		    (buffer[8] - '0');
		    
	printk ("PROMLIB: Sun IEEE Boot Prom %s\n", buffer + 4);
	
	prom_meminit();

	prom_ranges_init();

	/* Initialization successful. */
}