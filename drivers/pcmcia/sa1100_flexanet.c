/*
 * drivers/pcmcia/sa1100_flexanet.c
 *
 * PCMCIA implementation routines for Flexanet.
 * by Jordi Colomer, 09/05/2001
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/init.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/irq.h>
#include "sa1100_generic.h"

static struct {
  int irq;
  const char *name;
} irqs[] = {
  { IRQ_GPIO_CF1_CD,   "CF1_CD"   },
  { IRQ_GPIO_CF1_BVD1, "CF1_BVD1" },
  { IRQ_GPIO_CF2_CD,   "CF2_CD"   },
  { IRQ_GPIO_CF2_BVD1, "CF2_BVD1" }
};

/*
 * Socket initialization.
 *
 * Called by sa1100_pcmcia_driver_init on startup.
 * Must return the number of slots.
 *
 */
static int flexanet_pcmcia_init(struct pcmcia_init *init)
{
  int i, res;

  /* Configure the GPIOs as inputs (BVD2 is not implemented) */
  GPDR &= ~(GPIO_CF1_NCD | GPIO_CF1_BVD1 | GPIO_CF1_IRQ |
            GPIO_CF2_NCD | GPIO_CF2_BVD1 | GPIO_CF2_IRQ );

  /* Register the socket interrupts (not the card interrupts) */
  for (i = 0; i < ARRAY_SIZE(irqs); i++) {
    res = request_irq(irqs[i].irq, sa1100_pcmcia_interrupt, SA_INTERRUPT,
		      irqs[i].name, NULL);
    if (res < 0)
      break;
    set_irq_type(irqs[i].irq, IRQT_NOEDGE);
  }

  init->socket_irq[0] = IRQ_GPIO_CF1_IRQ;
  init->socket_irq[1] = IRQ_GPIO_CF2_IRQ;

  /* If we failed, then free all interrupts requested thus far. */
  if (res < 0) {
    printk(KERN_ERR "%s: request for IRQ%d failed: %d\n",
	   __FUNCTION__, irqs[i].irq, res);
    while (i--)
      free_irq(irqs[i].irq, NULL);
    return res;
  }

  return 2;
}


/*
 * Socket shutdown
 *
 */
static int flexanet_pcmcia_shutdown(void)
{
  int i;

  /* disable IRQs */
  for (i = 0; i < ARRAY_SIZE(irqs); i++)
    free_irq(irqs[i].irq, NULL);

  return 0;
}


/*
 * Get the state of the sockets.
 *
 *  Sockets in Flexanet are 3.3V only, without BVD2.
 *
 */
static void flexanet_pcmcia_socket_state(int sock, struct pcmcia_state *state)
{
  unsigned long levels = GPLR; /* Sense the GPIOs, asynchronously */

  switch (sock) {
  case 0: /* Socket 0 */
    state->detect = ((levels & GPIO_CF1_NCD)==0)?1:0;
    state->ready  = (levels & GPIO_CF1_IRQ)?1:0;
    state->bvd1   = (levels & GPIO_CF1_BVD1)?1:0;
    state->bvd2   = 1;
    state->wrprot = 0;
    state->vs_3v  = 1;
    state->vs_Xv  = 0;
    break;

  case 1: /* Socket 1 */
    state->detect = ((levels & GPIO_CF2_NCD)==0)?1:0;
    state->ready  = (levels & GPIO_CF2_IRQ)?1:0;
    state->bvd1   = (levels & GPIO_CF2_BVD1)?1:0;
    state->bvd2   = 1;
    state->wrprot = 0;
    state->vs_3v  = 1;
    state->vs_Xv  = 0;
    break;
  }
}


/*
 *
 */
static int flexanet_pcmcia_configure_socket(int sock, const struct pcmcia_configure
					   *configure)
{
  unsigned long value, flags, mask;

  if (sock > 1)
    return -1;

  /* Ignore the VCC level since it is 3.3V and always on */
  switch (configure->vcc)
  {
    case 0:
      printk(KERN_WARNING "%s(): CS asked to power off.\n", __FUNCTION__);
      break;

    case 50:
      printk(KERN_WARNING "%s(): CS asked for 5V, applying 3.3V...\n",
  	   __FUNCTION__);

    case 33:
      break;

    default:
      printk(KERN_ERR "%s(): unrecognized Vcc %u\n", __FUNCTION__,
  	   configure->vcc);
      return -1;
  }

  /* Reset the slot(s) using the controls in the BCR */
  mask = 0;

  switch (sock)
  {
    case 0 : mask = FHH_BCR_CF1_RST; break;
    case 1 : mask = FHH_BCR_CF2_RST; break;
  }

  local_irq_save(flags);

  value = flexanet_BCR;
  value = (configure->reset) ? (value | mask) : (value & ~mask);
  FHH_BCR = flexanet_BCR = value;

  local_irq_restore(flags);

  return 0;
}

static int flexanet_pcmcia_socket_init(int sock)
{
  if (sock == 0) {
    set_irq_type(IRQ_GPIO_CF1_CD, IRQT_BOTHEDGE);
    set_irq_type(IRQ_GPIO_CF1_BVD1, IRQT_BOTHEDGE);
  } else if (sock == 1) {
    set_irq_type(IRQ_GPIO_CF2_CD, IRQT_BOTHEDGE);
    set_irq_type(IRQ_GPIO_CF2_BVD1, IRQT_BOTHEDGE);
  }

  return 0;
}

static int flexanet_pcmcia_socket_suspend(int sock)
{
  if (sock == 0) {
    set_irq_type(IRQ_GPIO_CF1_CD, IRQT_NOEDGE);
    set_irq_type(IRQ_GPIO_CF1_BVD1, IRQT_NOEDGE);
  } else if (sock == 1) {
    set_irq_type(IRQ_GPIO_CF2_CD, IRQT_NOEDGE);
    set_irq_type(IRQ_GPIO_CF2_BVD1, IRQT_NOEDGE);
  }

  return 0;
}

/*
 * The set of socket operations
 *
 */
static struct pcmcia_low_level flexanet_pcmcia_ops = {
  .owner		= THIS_MODULE,
  .init			= flexanet_pcmcia_init,
  .shutdown		= flexanet_pcmcia_shutdown,
  .socket_state		= flexanet_pcmcia_socket_state,
  .configure_socket	= flexanet_pcmcia_configure_socket,

  .socket_init		= flexanet_pcmcia_socket_init,
  .socket_suspend	= flexanet_pcmcia_socket_suspend,
};

int __init pcmcia_flexanet_init(struct device *dev)
{
	int ret = -ENODEV;

	if (machine_is_flexanet())
		ret = sa1100_register_pcmcia(&flexanet_pcmcia_ops, dev);

	return ret;
}

void __exit pcmcia_flexanet_exit(struct device *dev)
{
	sa1100_unregister_pcmcia(&flexanet_pcmcia_ops, dev);
}

