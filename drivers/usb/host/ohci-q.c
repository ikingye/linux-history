/*
 * OHCI HCD (Host Controller Driver) for USB.
 * 
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * 
 * This file is licenced under the GPL.
 * $Id: ohci-q.c,v 1.8 2002/03/27 20:57:01 dbrownell Exp $
 */

static void urb_free_priv (struct ohci_hcd *hc, urb_priv_t *urb_priv)
{
	int		last = urb_priv->length - 1;

	if (last >= 0) {
		int		i;
		struct td	*td = urb_priv->td [0];
		int		len = td->urb->transfer_buffer_length;
		int		dir = usb_pipeout (td->urb->pipe)
					? PCI_DMA_TODEVICE
					: PCI_DMA_FROMDEVICE;

		/* unmap CTRL URB setup buffer (always td 0) */
		if (usb_pipecontrol (td->urb->pipe)) {
			pci_unmap_single (hc->hcd.pdev, 
					td->data_dma, 8, PCI_DMA_TODEVICE);
			
			/* CTRL data buffer starts at td 1 if len > 0 */
			if (len && last > 0)
				td = urb_priv->td [1]; 		
		}
		/* else:  ISOC, BULK, INTR data buffer starts at td 0 */

		/* unmap data buffer */
		if (len && td->data_dma)
			pci_unmap_single (hc->hcd.pdev,
					td->data_dma, len, dir);
		for (i = 0; i <= last; i++) {
			td = urb_priv->td [i];
			if (td)
				td_free (hc, td);
		}
	}

	kfree (urb_priv);
}

/*-------------------------------------------------------------------------*/

/*
 * URB goes back to driver, and isn't reissued.
 * It's completely gone from HC data structures.
 * PRECONDITION:  no locks held  (Giveback can call into HCD.)
 */
static void finish_urb (struct ohci_hcd *ohci, struct urb *urb)
{
	unsigned long	flags;

#ifdef DEBUG
	if (!urb->hcpriv) {
		err ("already unlinked!");
		BUG ();
	}
#endif

	urb_free_priv (ohci, urb->hcpriv);
	urb->hcpriv = NULL;

	spin_lock_irqsave (&urb->lock, flags);
	if (likely (urb->status == -EINPROGRESS))
		urb->status = 0;
	spin_unlock_irqrestore (&urb->lock, flags);

#ifdef OHCI_VERBOSE_DEBUG
	urb_print (urb, "RET", usb_pipeout (urb->pipe));
#endif
	usb_hcd_giveback_urb (&ohci->hcd, urb);
}

static void td_submit_urb (struct urb *urb);

/* Report interrupt transfer completion, maybe reissue */
static void intr_resub (struct ohci_hcd *hc, struct urb *urb)
{
	urb_priv_t	*urb_priv = urb->hcpriv;
	unsigned long	flags;

// FIXME rewrite this resubmit path.  use pci_dma_sync_single()
// and requeue more cheaply, and only if needed.
// Better yet ... abolish the notion of automagic resubmission.
	pci_unmap_single (hc->hcd.pdev,
		urb_priv->td [0]->data_dma,
		urb->transfer_buffer_length,
		usb_pipeout (urb->pipe)
			? PCI_DMA_TODEVICE
			: PCI_DMA_FROMDEVICE);
	/* FIXME: MP race.  If another CPU partially unlinks
	 * this URB (urb->status was updated, hasn't yet told
	 * us to dequeue) before we call complete() here, an
	 * extra "unlinked" completion will be reported...
	 */

	spin_lock_irqsave (&urb->lock, flags);
	if (likely (urb->status == -EINPROGRESS))
		urb->status = 0;
	spin_unlock_irqrestore (&urb->lock, flags);

#ifdef OHCI_VERBOSE_DEBUG
	urb_print (urb, "INTR", usb_pipeout (urb->pipe));
#endif
	urb->complete (urb);

	/* always requeued, but ED_SKIP if complete() unlinks.
	 * EDs are removed from periodic table only at SOF intr.
	 */
	urb->actual_length = 0;
	spin_lock_irqsave (&urb->lock, flags);
	if (urb_priv->state != URB_DEL)
		urb->status = -EINPROGRESS;
	spin_unlock (&urb->lock);

	spin_lock (&hc->lock);
	td_submit_urb (urb);
	spin_unlock_irqrestore (&hc->lock, flags);
}


/*-------------------------------------------------------------------------*
 * ED handling functions
 *-------------------------------------------------------------------------*/  

/* search for the right branch to insert an interrupt ed into the int tree 
 * do some load balancing;
 * returns the branch
 * FIXME allow for failure, when there's no bandwidth left;
 * and consider iso loads too
 */
static int ep_int_balance (struct ohci_hcd *ohci, int interval, int load)
{
	int	i, branch = 0;

	/* search for the least loaded interrupt endpoint branch */
	for (i = 0; i < NUM_INTS ; i++) 
		if (ohci->ohci_int_load [branch] > ohci->ohci_int_load [i])
			branch = i; 

	branch = branch % interval;
	for (i = branch; i < NUM_INTS; i += interval)
		ohci->ohci_int_load [i] += load;

	return branch;
}

/*-------------------------------------------------------------------------*/

/* the int tree is a binary tree 
 * in order to process it sequentially the indexes of the branches have
 * to be mapped the mapping reverses the bits of a word of num_bits length
 */
static int ep_rev (int num_bits, int word)
{
	int			i, wout = 0;

	for (i = 0; i < num_bits; i++)
		wout |= (( (word >> i) & 1) << (num_bits - i - 1));
	return wout;
}

/*-------------------------------------------------------------------------*/

/* link an ed into one of the HC chains */

static int ep_link (struct ohci_hcd *ohci, struct ed *edi)
{	 
	int			int_branch, i;
	int			inter, interval, load;
	__u32			*ed_p;
	volatile struct ed	*ed = edi;

	ed->state = ED_OPER;

	switch (ed->type) {
	case PIPE_CONTROL:
		ed->hwNextED = 0;
		if (ohci->ed_controltail == NULL) {
			writel (ed->dma, &ohci->regs->ed_controlhead);
		} else {
			ohci->ed_controltail->hwNextED = cpu_to_le32 (ed->dma);
		}
		ed->ed_prev = ohci->ed_controltail;
		if (!ohci->ed_controltail
				&& !ohci->ed_rm_list
				&& !ohci->sleeping
				) {
			ohci->hc_control |= OHCI_CTRL_CLE;
			writel (ohci->hc_control, &ohci->regs->control);
		}
		ohci->ed_controltail = edi;	  
		break;

	case PIPE_BULK:
		ed->hwNextED = 0;
		if (ohci->ed_bulktail == NULL) {
			writel (ed->dma, &ohci->regs->ed_bulkhead);
		} else {
			ohci->ed_bulktail->hwNextED = cpu_to_le32 (ed->dma);
		}
		ed->ed_prev = ohci->ed_bulktail;
		if (!ohci->ed_bulktail
				&& !ohci->ed_rm_list
				&& !ohci->sleeping
				) {
			ohci->hc_control |= OHCI_CTRL_BLE;
			writel (ohci->hc_control, &ohci->regs->control);
		}
		ohci->ed_bulktail = edi;	  
		break;

	case PIPE_INTERRUPT:
		load = ed->intriso.intr_info.int_load;
		interval = ed->interval;
		int_branch = ep_int_balance (ohci, interval, load);
		ed->intriso.intr_info.int_branch = int_branch;

		for (i = 0; i < ep_rev (6, interval); i += inter) {
			inter = 1;
			for (ed_p = & (ohci->hcca->int_table [ep_rev (5, i) + int_branch]); 
				 (*ed_p != 0) && ((dma_to_ed (ohci, le32_to_cpup (ed_p)))->interval >= interval); 
				ed_p = & ((dma_to_ed (ohci, le32_to_cpup (ed_p)))->hwNextED)) 
					inter = ep_rev (6, (dma_to_ed (ohci, le32_to_cpup (ed_p)))->interval);
			ed->hwNextED = *ed_p; 
			*ed_p = cpu_to_le32 (ed->dma);
		}
#ifdef OHCI_VERBOSE_DEBUG
		ohci_dump_periodic (ohci, "LINK_INT");
#endif
		break;

	case PIPE_ISOCHRONOUS:
		ed->hwNextED = 0;
		ed->interval = 1;
		if (ohci->ed_isotail != NULL) {
			ohci->ed_isotail->hwNextED = cpu_to_le32 (ed->dma);
			ed->ed_prev = ohci->ed_isotail;
		} else {
			for ( i = 0; i < NUM_INTS; i += inter) {
				inter = 1;
				for (ed_p = & (ohci->hcca->int_table [ep_rev (5, i)]); 
					*ed_p != 0; 
					ed_p = & ((dma_to_ed (ohci, le32_to_cpup (ed_p)))->hwNextED)) 
						inter = ep_rev (6, (dma_to_ed (ohci, le32_to_cpup (ed_p)))->interval);
				*ed_p = cpu_to_le32 (ed->dma);	
			}	
			ed->ed_prev = NULL;
		}	
		ohci->ed_isotail = edi;  
#ifdef OHCI_VERBOSE_DEBUG
		ohci_dump_periodic (ohci, "LINK_ISO");
#endif
		break;
	}	 	
	return 0;
}

/*-------------------------------------------------------------------------*/

/* scan the periodic table to find and unlink this ED */
static void periodic_unlink (
	struct ohci_hcd	*ohci,
	struct ed	*ed,
	unsigned	index,
	unsigned	period
) {
	for (; index < NUM_INTS; index += period) {
		__u32	*ed_p = &ohci->hcca->int_table [index];

		while (*ed_p != 0) {
			if ((dma_to_ed (ohci, le32_to_cpup (ed_p))) == ed) {
				*ed_p = ed->hwNextED;		
				break;
			}
			ed_p = & ((dma_to_ed (ohci, le32_to_cpup (ed_p)))->hwNextED);
		}
	}	
}

/* unlink an ed from one of the HC chains. 
 * just the link to the ed is unlinked.
 * the link from the ed still points to another operational ed or 0
 * so the HC can eventually finish the processing of the unlinked ed
 * caller guarantees the ED has no active TDs.
 */
static int start_ed_unlink (struct ohci_hcd *ohci, struct ed *ed) 
{
	int	i;

	ed->hwINFO |= ED_SKIP;

	switch (ed->type) {
	case PIPE_CONTROL:
		if (ed->ed_prev == NULL) {
			if (!ed->hwNextED) {
				ohci->hc_control &= ~OHCI_CTRL_CLE;
				writel (ohci->hc_control, &ohci->regs->control);
			}
			writel (le32_to_cpup (&ed->hwNextED),
				&ohci->regs->ed_controlhead);
		} else {
			ed->ed_prev->hwNextED = ed->hwNextED;
		}
		if (ohci->ed_controltail == ed) {
			ohci->ed_controltail = ed->ed_prev;
		} else {
			 (dma_to_ed (ohci, le32_to_cpup (&ed->hwNextED)))
			 	->ed_prev = ed->ed_prev;
		}
		break;

	case PIPE_BULK:
		if (ed->ed_prev == NULL) {
			if (!ed->hwNextED) {
				ohci->hc_control &= ~OHCI_CTRL_BLE;
				writel (ohci->hc_control, &ohci->regs->control);
			}
			writel (le32_to_cpup (&ed->hwNextED),
				&ohci->regs->ed_bulkhead);
		} else {
			ed->ed_prev->hwNextED = ed->hwNextED;
		}
		if (ohci->ed_bulktail == ed) {
			ohci->ed_bulktail = ed->ed_prev;
		} else {
			 (dma_to_ed (ohci, le32_to_cpup (&ed->hwNextED)))
			 	->ed_prev = ed->ed_prev;
		}
		break;

	case PIPE_INTERRUPT:
		periodic_unlink (ohci, ed, ed->intriso.intr_info.int_branch, ed->interval);
		for (i = ed->intriso.intr_info.int_branch; i < NUM_INTS; i += ed->interval)
			ohci->ohci_int_load [i] -= ed->intriso.intr_info.int_load;
#ifdef OHCI_VERBOSE_DEBUG
		ohci_dump_periodic (ohci, "UNLINK_INT");
#endif
		break;

	case PIPE_ISOCHRONOUS:
		if (ohci->ed_isotail == ed)
			ohci->ed_isotail = ed->ed_prev;
		if (ed->hwNextED != 0) 
			(dma_to_ed (ohci, le32_to_cpup (&ed->hwNextED)))
		    		->ed_prev = ed->ed_prev;

		if (ed->ed_prev != NULL)
			ed->ed_prev->hwNextED = ed->hwNextED;
		else
			periodic_unlink (ohci, ed, 0, 1);
#ifdef OHCI_VERBOSE_DEBUG
		ohci_dump_periodic (ohci, "UNLINK_ISO");
#endif
		break;
	}

	/* FIXME ED's "unlink" state is indeterminate;
	 * the HC might still be caching it (till SOF).
	 * - use ed_rm_list and finish_unlinks(), adding some state that
	 *   prevents clobbering hw linkage before the appropriate SOF
	 * - a speedup:  when only one urb is queued on the ed, save 1msec
	 *   by making start_urb_unlink() use this routine to deschedule.
	 */
	ed->state = ED_UNLINK;
	return 0;
}


/*-------------------------------------------------------------------------*/

/* get and maybe (re)init an endpoint. init _should_ be done only as part
 * of usb_set_configuration() or usb_set_interface() ... but the USB stack
 * isn't very stateful, so we re-init whenever the HC isn't looking.
 */
static struct ed *ed_get (
	struct ohci_hcd		*ohci,
	struct usb_device	*udev,
	unsigned int		pipe,
	int			interval
) {
	int			is_out = !usb_pipein (pipe);
	int			type = usb_pipetype (pipe);
	int			bus_msecs = 0;
	struct hcd_dev		*dev = (struct hcd_dev *) udev->hcpriv;
	struct ed		*ed; 
	unsigned		ep;
	unsigned long		flags;

	ep = usb_pipeendpoint (pipe) << 1;
	if (type != PIPE_CONTROL && is_out)
		ep |= 1;
	if (type == PIPE_INTERRUPT)
		bus_msecs = usb_calc_bus_time (udev->speed, !is_out, 0,
			usb_maxpacket (udev, pipe, is_out)) / 1000;

	spin_lock_irqsave (&ohci->lock, flags);

	if (!(ed = dev->ep [ep])) {
		ed = ed_alloc (ohci, SLAB_ATOMIC);
		if (!ed) {
			/* out of memory */
			goto done;
		}
		dev->ep [ep] = ed;
	}

	if (ed->state & ED_URB_DEL) {
		/* pending unlink request */
		ed = 0;
		goto done;
	}

	if (ed->state == ED_NEW) {
		struct td		*td;

		ed->hwINFO = ED_SKIP;
  		/* dummy td; end of td list for ed */
		td = td_alloc (ohci, SLAB_ATOMIC);
 		if (!td) {
			/* out of memory */
			ed = 0;
			goto done;
		}
		ed->dummy = td;
		ed->hwTailP = cpu_to_le32 (td->td_dma);
		ed->hwHeadP = ed->hwTailP;	/* ED_C, ED_H zeroed */
		ed->state = ED_UNLINK;
		ed->type = type;
	}

	/* FIXME:  Don't do this without knowing it's safe to clobber this
	 * state/mode info.  Currently the upper layers don't support such
	 * guarantees; we're lucky changing config/altsetting is rare.
	 */
  	if (ed->state == ED_UNLINK) {
		u32	info;

		info = usb_pipedevice (pipe);
		info |= (ep >> 1) << 7;
		info |= usb_maxpacket (udev, pipe, is_out) << 16;
		info = cpu_to_le32 (info);
		if (udev->speed == USB_SPEED_LOW)
			info |= ED_LOWSPEED;
		/* control transfers store pids in tds */
		if (type != PIPE_CONTROL) {
			info |= is_out ? ED_OUT : ED_IN;
			if (type == PIPE_ISOCHRONOUS)
				info |= ED_ISO;
			if (type == PIPE_INTERRUPT) {
				ed->intriso.intr_info.int_load = bus_msecs;
				if (interval > 32)
					interval = 32;
			}
		}
		ed->hwINFO = info;

		/* value ignored except on periodic EDs, where
		 * we know it's already a power of 2
		 */
		ed->interval = interval;
	}

done:
	spin_unlock_irqrestore (&ohci->lock, flags);
	return ed; 
}

/*-------------------------------------------------------------------------*/

/* request unlinking of an endpoint from an operational HC.
 * put the ep on the rm_list and stop the bulk or ctrl list 
 * real work is done at the next start frame (SF) hardware interrupt
 */
static void start_urb_unlink (struct ohci_hcd *ohci, struct ed *ed)
{    
	/* already pending? */
	if (ed->state & ED_URB_DEL)
		return;
	ed->state |= ED_URB_DEL;

	ed->hwINFO |= ED_SKIP;

	switch (ed->type) {
		case PIPE_CONTROL: /* stop control list */
			ohci->hc_control &= ~OHCI_CTRL_CLE;
			writel (ohci->hc_control,
				&ohci->regs->control); 
			break;
		case PIPE_BULK: /* stop bulk list */
			ohci->hc_control &= ~OHCI_CTRL_BLE;
			writel (ohci->hc_control,
				&ohci->regs->control); 
			break;
	}

	/* SF interrupt might get delayed; record the frame counter value that
	 * indicates when the HC isn't looking at it, so concurrent unlinks
	 * behave.  frame_no wraps every 2^16 msec, and changes right before
	 * SF is triggered.
	 */
	ed->tick = le16_to_cpu (ohci->hcca->frame_no) + 1;

	ed->ed_rm_list = ohci->ed_rm_list;
	ohci->ed_rm_list = ed;

	/* enable SOF interrupt */
	if (!ohci->sleeping) {
		writel (OHCI_INTR_SF, &ohci->regs->intrstatus);
		writel (OHCI_INTR_SF, &ohci->regs->intrenable);
	}
}

/*-------------------------------------------------------------------------*
 * TD handling functions
 *-------------------------------------------------------------------------*/

/* enqueue next TD for this URB (OHCI spec 5.2.8.2) */

static void
td_fill (struct ohci_hcd *ohci, unsigned int info,
	dma_addr_t data, int len,
	struct urb *urb, int index)
{
	struct td		*td, *td_pt;
	urb_priv_t		*urb_priv = urb->hcpriv;
	int			is_iso = info & TD_ISO;

	if (index >= urb_priv->length) {
		err ("internal OHCI error: TD index > length");
		return;
	}

	/* aim for only one interrupt per urb.  mostly applies to control
	 * and iso; other urbs rarely need more than one TD per urb.
	 *
	 * NOTE: could delay interrupts even for the last TD, and get fewer
	 * interrupts ... increasing per-urb latency by sharing interrupts.
	 */
	if (index != (urb_priv->length - 1))
		info |= is_iso ? TD_DI_SET (7) : TD_DI_SET (1);

	/* use this td as the next dummy */
	td_pt = urb_priv->td [index];
	td_pt->hwNextTD = 0;

	/* fill the old dummy TD */
	td = urb_priv->td [index] = urb_priv->ed->dummy;
	urb_priv->ed->dummy = td_pt;

	td->ed = urb_priv->ed;
	td->next_dl_td = NULL;
	td->index = index;
	td->urb = urb; 
	td->data_dma = data;
	if (!len)
		data = 0;

	td->hwINFO = cpu_to_le32 (info);
	if (is_iso) {
		td->hwCBP = cpu_to_le32 (data & 0xFFFFF000);
		td->ed->intriso.last_iso = info & 0xffff;
	} else {
		td->hwCBP = cpu_to_le32 (data); 
	}			
	if (data)
		td->hwBE = cpu_to_le32 (data + len - 1);
	else
		td->hwBE = 0;
	td->hwNextTD = cpu_to_le32 (td_pt->td_dma);
	td->hwPSW [0] = cpu_to_le16 ((data & 0x0FFF) | 0xE000);

	/* HC might read the TD right after we link it ... */
	wmb ();

	/* append to queue */
	td->ed->hwTailP = td->hwNextTD;
}

/*-------------------------------------------------------------------------*/

/* prepare all TDs of a transfer */

static void td_submit_urb (struct urb *urb)
{ 
	urb_priv_t	*urb_priv = urb->hcpriv;
	struct ohci_hcd *ohci = hcd_to_ohci (urb->dev->bus->hcpriv);
	dma_addr_t	data;
	int		data_len = urb->transfer_buffer_length;
	int		cnt = 0; 
	__u32		info = 0;
  	unsigned int	toggle = 0;

	/* OHCI handles the DATA-toggles itself, we just use the
	 * USB-toggle bits for resetting
	 */
  	if (usb_gettoggle (urb->dev, usb_pipeendpoint (urb->pipe),
			usb_pipeout (urb->pipe))) {
  		toggle = TD_T_TOGGLE;
	} else {
  		toggle = TD_T_DATA0;
		usb_settoggle (urb->dev, usb_pipeendpoint (urb->pipe),
			usb_pipeout (urb->pipe), 1);
	}

	urb_priv->td_cnt = 0;

	if (data_len) {
		data = pci_map_single (ohci->hcd.pdev,
				       urb->transfer_buffer, data_len,
				       usb_pipeout (urb->pipe)
				       ? PCI_DMA_TODEVICE
				       : PCI_DMA_FROMDEVICE);
	} else
		data = 0;

	/* NOTE:  TD_CC is set so we can tell which TDs the HC processed by
	 * using TD_CC_GET, as well as by seeing them on the done list.
	 */
	switch (usb_pipetype (urb->pipe)) {
		case PIPE_BULK:
			info = usb_pipeout (urb->pipe)
				? TD_CC | TD_DP_OUT
				: TD_CC | TD_DP_IN ;
			while (data_len > 4096) {		
				td_fill (ohci,
					info | (cnt? TD_T_TOGGLE:toggle),
					data, 4096, urb, cnt);
				data += 4096; data_len -= 4096; cnt++;
			}
			info = usb_pipeout (urb->pipe)?
				TD_CC | TD_DP_OUT : TD_CC | TD_R | TD_DP_IN ;
			td_fill (ohci, info | (cnt? TD_T_TOGGLE:toggle),
				data, data_len, urb, cnt);
			cnt++;
			if ((urb->transfer_flags & USB_ZERO_PACKET)
					&& cnt < urb_priv->length) {
				td_fill (ohci, info | (cnt? TD_T_TOGGLE:toggle),
					0, 0, urb, cnt);
				cnt++;
			}
			/* start bulk list */
			if (!ohci->sleeping) {
				wmb ();
				writel (OHCI_BLF, &ohci->regs->cmdstatus);
			}
			break;

		case PIPE_INTERRUPT:
			info = TD_CC | toggle;
			info |= usb_pipeout (urb->pipe) 
				?  TD_DP_OUT
				:  TD_R | TD_DP_IN;
			td_fill (ohci, info, data, data_len, urb, cnt++);
			break;

		case PIPE_CONTROL:
			/* control requests don't use toggle state  */
			info = TD_CC | TD_DP_SETUP | TD_T_DATA0;
			td_fill (ohci, info,
				pci_map_single (ohci->hcd.pdev,
						urb->setup_packet, 8,
						PCI_DMA_TODEVICE),
				 8, urb, cnt++); 
			if (data_len > 0) {  
				info = TD_CC | TD_R | TD_T_DATA1;
				info |= usb_pipeout (urb->pipe)
				    ? TD_DP_OUT
				    : TD_DP_IN;
				/* NOTE:  mishandles transfers >8K, some >4K */
				td_fill (ohci, info, data, data_len,
						urb, cnt++);  
			} 
			info = usb_pipeout (urb->pipe)
				? TD_CC | TD_DP_IN | TD_T_DATA1
				: TD_CC | TD_DP_OUT | TD_T_DATA1;
			td_fill (ohci, info, data, 0, urb, cnt++);
			/* start control list */
			if (!ohci->sleeping) {
				wmb ();
				writel (OHCI_CLF, &ohci->regs->cmdstatus);
			}
			break;

		case PIPE_ISOCHRONOUS:
			for (cnt = 0; cnt < urb->number_of_packets; cnt++) {
				int	frame = urb->start_frame;

				// FIXME scheduling should handle frame counter
				// roll-around ... exotic case (and OHCI has
				// a 2^16 iso range, vs other HCs max of 2^10)
				frame += cnt * urb->interval;
				frame &= 0xffff;
				td_fill (ohci, TD_CC | TD_ISO | frame,
				    data + urb->iso_frame_desc [cnt].offset, 
				    urb->iso_frame_desc [cnt].length, urb, cnt); 
			}
			break;
	} 
	if (urb_priv->length != cnt) 
		dbg ("TD LENGTH %d != CNT %d", urb_priv->length, cnt);
}

/*-------------------------------------------------------------------------*
 * Done List handling functions
 *-------------------------------------------------------------------------*/

/* calculate transfer length/status and update the urb
 * PRECONDITION:  irqsafe (only for urb->status locking)
 */
static void td_done (struct urb *urb, struct td *td)
{
	u32	tdINFO = le32_to_cpup (&td->hwINFO);
	int	cc = 0;


	/* ISO ... drivers see per-TD length/status */
  	if (tdINFO & TD_ISO) {
 		u16	tdPSW = le16_to_cpu (td->hwPSW [0]);
		int	dlen = 0;

 		cc = (tdPSW >> 12) & 0xF;
		if (usb_pipeout (urb->pipe))
			dlen = urb->iso_frame_desc [td->index].length;
		else
			dlen = tdPSW & 0x3ff;
		urb->actual_length += dlen;
		urb->iso_frame_desc [td->index].actual_length = dlen;
		urb->iso_frame_desc [td->index].status = cc_to_error [cc];

		if (cc != 0)
			dbg ("  urb %p iso TD %p (%d) len %d CC %d",
				urb, td, 1 + td->index, dlen, cc);

	/* BULK, INT, CONTROL ... drivers see aggregate length/status,
	 * except that "setup" bytes aren't counted and "short" transfers
	 * might not be reported as errors.
	 */
	} else {
		int	type = usb_pipetype (urb->pipe);
		u32	tdBE = le32_to_cpup (&td->hwBE);

  		cc = TD_CC_GET (tdINFO);

		/* control endpoints only have soft stalls */
  		if (type != PIPE_CONTROL && cc == TD_CC_STALL)
			usb_endpoint_halt (urb->dev,
				usb_pipeendpoint (urb->pipe),
				usb_pipeout (urb->pipe));

		/* update packet status if needed (short is normally ok) */
		if (cc == TD_DATAUNDERRUN
				&& !(urb->transfer_flags & URB_SHORT_NOT_OK))
			cc = TD_CC_NOERROR;
		if (cc != TD_CC_NOERROR) {
			spin_lock (&urb->lock);
			if (urb->status == -EINPROGRESS)
				urb->status = cc_to_error [cc];
			spin_unlock (&urb->lock);
		}

		/* count all non-empty packets except control SETUP packet */
		if ((type != PIPE_CONTROL || td->index != 0) && tdBE != 0) {
			if (td->hwCBP == 0)
				urb->actual_length += tdBE - td->data_dma + 1;
			else
				urb->actual_length +=
					  le32_to_cpup (&td->hwCBP)
					- td->data_dma;
		}

#ifdef VERBOSE_DEBUG
		if (cc != 0)
			dbg ("  urb %p TD %p (%d) CC %d, len=%d/%d",
				urb, td, 1 + td->index, cc,
				urb->actual_length,
				urb->transfer_buffer_length);
#endif
  	}
}

/*-------------------------------------------------------------------------*/

/* replies to the request have to be on a FIFO basis so
 * we unreverse the hc-reversed done-list
 */
static struct td *dl_reverse_done_list (struct ohci_hcd *ohci)
{
	__u32		td_list_hc;
	struct td	*td_rev = NULL;
	struct td	*td_list = NULL;
  	urb_priv_t	*urb_priv = NULL;
  	unsigned long	flags;

  	spin_lock_irqsave (&ohci->lock, flags);

	td_list_hc = le32_to_cpup (&ohci->hcca->done_head);
	ohci->hcca->done_head = 0;

	while (td_list_hc) {		
		td_list = dma_to_td (ohci, td_list_hc);

		if (TD_CC_GET (le32_to_cpup (&td_list->hwINFO))) {
			urb_priv = (urb_priv_t *) td_list->urb->hcpriv;
			/* typically the endpoint halts on error; un-halt,
			 * and maybe dequeue other TDs from this urb
			 */
			if (td_list->ed->hwHeadP & ED_H) {
				if (urb_priv && ((td_list->index + 1)
						< urb_priv->length)) {
#ifdef OHCI_VERBOSE_DEBUG
					dbg ("urb %p TD %p (%d/%d), patch ED",
						td_list->urb, td_list,
						1 + td_list->index,
						urb_priv->length);
#endif
					td_list->ed->hwHeadP = 
			    (urb_priv->td [urb_priv->length - 1]->hwNextTD
				    & __constant_cpu_to_le32 (TD_MASK))
			    | (td_list->ed->hwHeadP & ED_C);
					urb_priv->td_cnt += urb_priv->length
						- td_list->index - 1;
				} else 
					td_list->ed->hwHeadP &= ~ED_H;
			}
		}

		td_list->next_dl_td = td_rev;	
		td_rev = td_list;
		td_list_hc = le32_to_cpup (&td_list->hwNextTD);
	}	
	spin_unlock_irqrestore (&ohci->lock, flags);
	return td_list;
}

/*-------------------------------------------------------------------------*/

/* wrap-aware logic stolen from <linux/jiffies.h> */
#define tick_before(t1,t2) ((((s16)(t1))-((s16)(t2))) < 0)

/* there are some urbs/eds to unlink; called in_irq(), with HCD locked */
static void finish_unlinks (struct ohci_hcd *ohci, u16 tick)
{
	struct ed	*ed, **last;
	int		ctrl = 0, bulk = 0;

	for (last = &ohci->ed_rm_list, ed = *last; ed != NULL; ed = *last) {
		struct td	*td, *td_next, *tdHeadP, *tdTailP;
		u32		*td_p;
		int		unlinked;

		/* only take off EDs that the HC isn't using, accounting for
		 * frame counter wraps.  completion callbacks might prepend
		 * EDs to the list, they'll be checked next irq.
		 */
		if (tick_before (tick, ed->tick)) {
			last = &ed->ed_rm_list;
			continue;
		}
		*last = ed->ed_rm_list;
		ed->ed_rm_list = 0;
		unlinked = 0;

		/* unlink urbs from first one requested to queue end;
		 * leave earlier urbs alone
		 */
		tdTailP = dma_to_td (ohci, le32_to_cpup (&ed->hwTailP));
		tdHeadP = dma_to_td (ohci, le32_to_cpup (&ed->hwHeadP));
		td_p = &ed->hwHeadP;

		for (td = tdHeadP; td != tdTailP; td = td_next) {
			struct urb *urb = td->urb;
			urb_priv_t *urb_priv = td->urb->hcpriv;

			td_next = dma_to_td (ohci,
				le32_to_cpup (&td->hwNextTD));
			if (unlinked || (urb_priv->state == URB_DEL)) {
				u32 tdINFO = le32_to_cpup (&td->hwINFO);

				unlinked = 1;

				/* HC may have partly processed this TD */
				if (TD_CC_GET (tdINFO) < 0xE)
					td_done (urb, td);
				*td_p = td->hwNextTD | (*td_p
					& __constant_cpu_to_le32 (0x3));

				/* URB is done; clean up */
				if (++ (urb_priv->td_cnt) == urb_priv->length) {
					if (urb->status == -EINPROGRESS)
						urb->status = -ECONNRESET;
     					spin_unlock (&ohci->lock);
					finish_urb (ohci, urb);
					spin_lock (&ohci->lock);
				}
			} else {
				td_p = &td->hwNextTD;
			}
		}

		/* FIXME actually want four cases here:
		 * (a) finishing URB unlink
		 *     [a1] no URBs queued, so start ED unlink
		 *     [a2] some (earlier) URBs still linked, re-enable
		 * (b) finishing ED unlink
		 *     [b1] no URBs queued, ED is truly idle now
		 *          ... we could set state ED_NEW and free dummy
		 *     [b2] URBs now queued, link ED back into schedule
		 * right now we only have (a)
		 */
		ed->state &= ~ED_URB_DEL;
		tdHeadP = dma_to_td (ohci, le32_to_cpup (&ed->hwHeadP));

		if (tdHeadP == tdTailP) {
			if (ed->state == ED_OPER)
				start_ed_unlink (ohci, ed);
		} else
			ed->hwINFO &= ~ED_SKIP;

		switch (ed->type) {
			case PIPE_CONTROL:
				ctrl = 1;
				break;
			case PIPE_BULK:
				bulk = 1;
				break;
		}
   	}

	/* maybe reenable control and bulk lists */ 
	if (!ohci->disabled) {
		if (ctrl) 	/* reset control list */
			writel (0, &ohci->regs->ed_controlcurrent);
		if (bulk)	/* reset bulk list */
			writel (0, &ohci->regs->ed_bulkcurrent);
		if (!ohci->ed_rm_list) {
			if (ohci->ed_controltail)
				ohci->hc_control |= OHCI_CTRL_CLE;
			if (ohci->ed_bulktail)
				ohci->hc_control |= OHCI_CTRL_BLE;
			writel (ohci->hc_control, &ohci->regs->control);   
		}
	}
}



/*-------------------------------------------------------------------------*/

/*
 * Process normal completions (error or success) and clean the schedules.
 *
 * This is the main path for handing urbs back to drivers.  The only other
 * path is finish_unlinks(), which unlinks URBs using ed_rm_list, instead of
 * scanning the (re-reversed) donelist as this does.
 */
static void dl_done_list (struct ohci_hcd *ohci, struct td *td)
{
	unsigned long	flags;

  	spin_lock_irqsave (&ohci->lock, flags);
  	while (td) {
		struct td	*td_next = td->next_dl_td;
		struct urb	*urb = td->urb;
		urb_priv_t	*urb_priv = urb->hcpriv;
		struct ed	*ed = td->ed;

		/* update URB's length and status from TD */
   		td_done (urb, td);
  		urb_priv->td_cnt++;

		/* If all this urb's TDs are done, call complete().
		 * Interrupt transfers are the only special case:
		 * they're reissued, until "deleted" by usb_unlink_urb
		 * (real work done in a SOF intr, by finish_unlinks).
		 */
  		if (urb_priv->td_cnt == urb_priv->length) {
			int	resubmit;

			resubmit = usb_pipeint (urb->pipe)
					&& (urb_priv->state != URB_DEL);

     			spin_unlock_irqrestore (&ohci->lock, flags);
			if (resubmit)
  				intr_resub (ohci, urb);
  			else
  				finish_urb (ohci, urb);
  			spin_lock_irqsave (&ohci->lock, flags);
  		}

		/* clean schedule:  unlink EDs that are no longer busy */
		if ((ed->hwHeadP & __constant_cpu_to_le32 (TD_MASK))
					== ed->hwTailP
				&& (ed->state == ED_OPER)) 
			start_ed_unlink (ohci, ed);
    		td = td_next;
  	}  
	spin_unlock_irqrestore (&ohci->lock, flags);
}
