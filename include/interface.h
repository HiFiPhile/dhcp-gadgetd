#ifndef DG_INTERFACE_H
#define DG_INTERFACE_H

#include <net/if.h>
#include <stddef.h>

#include "dg_types.h"

typedef struct {
	int ifindex;
	char ifname[IF_NAMESIZE];
	char usb_path[DG_MAX_PATH_LEN];
	char mac[18];
} DgInterfaceEntry;

int dg_interface_refresh(void);
int dg_interface_upsert_with_ifindex(const char *ifname, int ifindex);
int dg_interface_remove(int ifindex);
size_t dg_interface_count(void);
const DgInterfaceEntry *dg_interface_get(size_t idx);
const DgInterfaceEntry *dg_interface_by_ifindex(int ifindex);
const DgInterfaceEntry *dg_interface_by_name(const char *ifname);

#endif