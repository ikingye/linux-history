/*********************************************************************
 *                
 * Filename:      irlan_eth.h
 * Version:       
 * Description:   
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Thu Oct 15 08:36:58 1998
 * Modified at:   Fri Jan 29 15:20:45 1999
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1998 Dag Brattli, All Rights Reserved.
 *      
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 *  
 *     Neither Dag Brattli nor University of Troms� admit liability nor
 *     provide warranty for any of this software. This material is 
 *     provided "AS-IS" and at no charge.
 *     
 ********************************************************************/

#ifndef IRLAN_ETH_H
#define IRLAN_ETH_H

void irlan_eth_receive( void *instance, void *sap, struct sk_buff *skb);
int  irlan_eth_xmit( struct sk_buff *skb, struct device *dev);

void irlan_eth_flow_indication( void *instance, void *sap, LOCAL_FLOW flow);

void irlan_eth_set_multicast_list( struct device *dev);
struct enet_statistics *irlan_eth_get_stats(struct device *dev);

#endif
