#ifndef _IPX_H_
#define _IPX_H_
#include <linux/sockios.h>
#define IPX_NODE_LEN	6
#define IPX_MTU		576

struct sockaddr_ipx
{
	short sipx_family;
	unsigned long  sipx_network;
	unsigned char sipx_node[IPX_NODE_LEN];
	short sipx_port;
	unsigned char	sipx_type;
	unsigned char	sipx_zero;	/* 16 byte fill */
};

/*
 *	So we can fit the extra info for SIOCSIFADDR into the address nicely
 */
 
#define sipx_primary	sipx_port
#define sipx_internal	sipx_zero

typedef struct ipx_route_definition
{
	unsigned long ipx_network;
	unsigned long ipx_router_network;
	unsigned char ipx_router_node[IPX_NODE_LEN];
}	ipx_route_definition;

typedef struct ipx_interface_definition
{
	unsigned long ipx_network;
	unsigned char ipx_device[16];
	unsigned short ipx_dlink_type;
#define IPX_FRAME_NONE		0
#define IPX_FRAME_SNAP		1
#define IPX_FRAME_8022		2
#define IPX_FRAME_ETHERII	3
#define IPX_FRAME_8023		4
	unsigned char ipx_primary;
	unsigned char ipx_internal;
	unsigned char ipx_node[IPX_NODE_LEN];
}	ipx_interface_definition;
	
typedef struct ipx_config_data
{
	unsigned char	ipxcfg_auto_select_primary;
	unsigned char	ipxcfg_auto_create_interfaces;
}	ipx_config_data;

/*
 * OLD Route Definition for backware compatibility.
 */

struct ipx_route_def
{
	unsigned long ipx_network;
	unsigned long ipx_router_network;
#define IPX_ROUTE_NO_ROUTER	0
	unsigned char ipx_router_node[IPX_NODE_LEN];
	unsigned char ipx_device[16];
	unsigned short ipx_flags;
#define IPX_RT_SNAP		8
#define IPX_RT_8022		4
#define IPX_RT_BLUEBOOK		2
#define IPX_RT_ROUTED		1
};

#define SIOCAIPXITFCRT		(SIOCPROTOPRIVATE)
#define SIOCAIPXPRISLT		(SIOCPROTOPRIVATE+1)
#define SIOCIPXCFGDATA		(SIOCPROTOPRIVATE+2)
#endif

