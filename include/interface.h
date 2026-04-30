#ifndef DG_INTERFACE_H
#define DG_INTERFACE_H

#include <net/if.h>
#include <stddef.h>

#include "dg_types.h"
#include "hotplug.h"

typedef struct {
	int ifindex;
	char ifname[IF_NAMESIZE];
	char usb_path[DG_MAX_PATH_LEN];
	int sock_fd;
} DgInterfaceEntry;

int dg_interface_init(void);
int dg_interface_upsert(const DgHotplugEvent *evt);
int dg_interface_remove(const DgHotplugEvent *evt);
int dg_interface_is_up(const DgInterfaceEntry *entry);
size_t dg_interface_count(void);
const DgInterfaceEntry *dg_interface_get(size_t idx);
const DgInterfaceEntry *dg_interface_by_ifindex(int ifindex);

#endif