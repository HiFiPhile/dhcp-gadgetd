/* Hotplug events carry the driver, USB path and ifindex, so no gadget hardware is needed. */
#include "interface.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static DgHotplugEvent event(const char *ifname, int ifindex, const char *driver) {
    DgHotplugEvent evt;

    memset(&evt, 0, sizeof(evt));
    evt.action = DG_HOTPLUG_ADD;
    evt.ifindex = ifindex;
    snprintf(evt.ifname, sizeof(evt.ifname), "%s", ifname);
    snprintf(evt.devpath, sizeof(evt.devpath), "/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/net/%s", ifname);
    snprintf(evt.id_net_driver, sizeof(evt.id_net_driver), "%s", driver);
    return evt;
}

int main(void) {
    DgConfig cfg;
    DgHotplugEvent evt;

    /* Driver names no host uses, so the initial sysfs scan finds nothing. */
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.drivers[0], "dg_test_a");
    strcpy(cfg.drivers[1], "dg_test_b");
    cfg.driver_count = 2;
    assert(dg_interface_init(&cfg) == 0);
    assert(dg_interface_count() == 0);

    evt = event("tg0", 9001, "dg_test_a");
    assert(dg_interface_upsert(&evt) == 0);
    evt = event("tg1", 9002, "dg_test_b");
    assert(dg_interface_upsert(&evt) == 0);
    evt = event("tg2", 9003, "cdc_ether");
    assert(dg_interface_upsert(&evt) != 0);
    assert(dg_interface_count() == 2);
    assert(strcmp(dg_interface_by_ifindex(9001)->usb_path, "1-2:1.0") == 0);
    assert(dg_interface_by_ifindex(9003) == NULL);

    puts("interface tests passed");
    return 0;
}
