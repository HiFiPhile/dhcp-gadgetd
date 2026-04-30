#ifndef DG_DHCP_PROTO_H
#define DG_DHCP_PROTO_H

#include <stdint.h>

#include "allocator.h"
#include "dg_types.h"
#include "interface.h"

int dg_dhcp_proto_init(const DgConfig *cfg, DgAllocator *allocator);
int dg_dhcp_proto_drain_socket(const DgInterfaceEntry *entry, uint64_t now_ms);
void dg_dhcp_proto_shutdown(void);

#endif