#ifndef DG_BUS_H
#define DG_BUS_H

#include <poll.h>
#include <systemd/sd-bus.h>

#include "allocator.h"

#define DG_BUS_NAME "org.dhcp_gadget1"
#define DG_BUS_PATH "/org/dhcp_gadget1"
#define DG_BUS_INTERFACE "org.dhcp_gadget1.DHCPServer"

typedef struct DgBus DgBus;

/* A NULL connection opens the system bus; otherwise takes its own reference. */
int dg_bus_init(DgBus **out, const DgConfig *cfg, DgAllocator *allocator, sd_bus *connection);
int dg_bus_process(DgBus *bus);
int dg_bus_prepare_poll(DgBus *bus, struct pollfd *pfd, int *timeout_ms);
void dg_bus_cleanup(DgBus *bus);

#endif
