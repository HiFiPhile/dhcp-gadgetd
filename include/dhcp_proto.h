#ifndef DG_DHCP_PROTO_H
#define DG_DHCP_PROTO_H

#include <stdint.h>

#include "allocator.h"
#include "dg_types.h"
#include "interface.h"

int dg_dhcp_proto_init(const DgConfig *cfg, DgAllocator *allocator);
int dg_dhcp_proto_bind_interface(const DgInterfaceEntry *iface);
int dg_dhcp_proto_unbind_ifname(const char *ifname);
int dg_dhcp_proto_poll(int timeout_ms, uint64_t now_ms, int hotplug_fd, int *hotplug_ready);
void dg_dhcp_proto_shutdown(void);

#endif