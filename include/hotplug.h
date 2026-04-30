#ifndef DG_HOTPLUG_H
#define DG_HOTPLUG_H

#include "dg_types.h"

typedef enum {
	DG_HOTPLUG_ADD = 1,
	DG_HOTPLUG_MOVE = 2,
	DG_HOTPLUG_REMOVE = 3,
} DgHotplugAction;

typedef struct {
	DgHotplugAction action;
	int ifindex;
	char ifname[16];
	char devpath[DG_MAX_PATH_LEN];
	char id_net_driver[32];
} DgHotplugEvent;

int dg_hotplug_init(void);
int dg_hotplug_fd(void);
int dg_hotplug_next_event(DgHotplugEvent *out_event);
void dg_hotplug_shutdown(void);

#endif