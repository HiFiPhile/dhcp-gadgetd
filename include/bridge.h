#ifndef DG_BRIDGE_H
#define DG_BRIDGE_H

#include "dg_types.h"

/*
 * Creates or adopts cfg->bridge_name, gives it a MAC derived from the machine ID,
 * brings it up and makes server_ip/subnet_prefix its only IPv4 address.
 * The bridge is kept when the daemon stops so clients survive restarts.
 */
int dg_bridge_init(const DgConfig *cfg);
/* Enslaves a gadget port to the bridge and brings the port up; repeating it is harmless. */
int dg_bridge_add_port(int ifindex, const char *ifname);
void dg_bridge_shutdown(void);

#endif
