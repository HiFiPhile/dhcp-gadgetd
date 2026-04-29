#include "server.h"

#include <string.h>
#include <time.h>

#include "dhcp_proto.h"
#include "hotplug.h"
#include "interface.h"
#include "log.h"

static const char *dg_hotplug_action_name(DgHotplugAction action) {
    switch (action) {
    case DG_HOTPLUG_ADD:
        return "add";
    case DG_HOTPLUG_MOVE:
        return "move";
    case DG_HOTPLUG_REMOVE:
        return "remove";
    default:
        return "unknown";
    }
}

static uint64_t dg_now_monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

int dg_server_init(DgServer *server, const DgConfig *cfg) {
    if (server == NULL || cfg == NULL) {
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->config = *cfg;

    if (dg_interface_refresh() != 0) {
        dg_log(DG_LOG_WARN, "initial interface refresh returned error");
    }

    if (dg_allocator_init(&server->allocator, cfg) != 0) {
        dg_log(DG_LOG_ERROR, "server init failed: allocator");
        return -1;
    }

    if (dg_dhcp_proto_init(cfg, server->allocator) != 0) {
        dg_allocator_cleanup(server->allocator);
        server->allocator = NULL;
        dg_log(DG_LOG_ERROR, "server init failed: dhcp protocol");
        return -1;
    }

    if (dg_hotplug_init() != 0) {
        dg_dhcp_proto_shutdown();
        dg_allocator_cleanup(server->allocator);
        server->allocator = NULL;
        dg_log(DG_LOG_ERROR, "server init failed: hotplug");
        return -1;
    }

    server->running = 1;
    dg_log(DG_LOG_INFO, "server initialized");
    return 0;
}

int dg_server_tick(DgServer *server, int timeout_ms) {
    int poll_rc;
    int hotplug_ready = 0;

    if (server == NULL || !server->running) {
        return -1;
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    poll_rc = dg_dhcp_proto_poll(timeout_ms, dg_now_monotonic_ms(), dg_hotplug_fd(), &hotplug_ready);
    if (poll_rc != 0) {
        dg_log(DG_LOG_ERROR, "poll failed in dhcp protocol tick");
        return -1;
    }

    if (hotplug_ready) {
        while (1) {
            DgHotplugEvent evt;
            int evt_rc = dg_hotplug_next_event(&evt);
            if (evt_rc < 0) {
                dg_log(DG_LOG_WARN, "hotplug read returned error");
                break;
            }
            if (evt_rc == 0) {
                break;
            }

                 dg_log(DG_LOG_DEBUG, "hotplug event: action=%s if=%s idx=%d",
                     dg_hotplug_action_name(evt.action), evt.ifname, evt.ifindex);

            if (evt.action == DG_HOTPLUG_REMOVE) {
                (void)dg_dhcp_proto_unbind_ifname(evt.ifname);
                (void)dg_interface_remove(evt.ifindex);
                continue;
            }

            if (evt.action == DG_HOTPLUG_ADD || evt.action == DG_HOTPLUG_MOVE) {
                if (dg_interface_upsert_with_ifindex(evt.ifname, evt.ifindex) == 0) {
                    const DgInterfaceEntry *iface = dg_interface_by_name(evt.ifname);
                    dg_log(DG_LOG_DEBUG, "hotplug upserted interface: if=%s idx=%d", evt.ifname, iface != NULL ? iface->ifindex : -1);
                    if (iface != NULL) {
                        (void)dg_dhcp_proto_bind_interface(iface);
                    }
                }
            }
        }
    } else if (server->tick_count % 20U == 0U) {
        // for testing
        /* if (dg_interface_refresh() != 0) {
            dg_log(DG_LOG_WARN, "periodic interface refresh returned error");
        } */
    }

    dg_allocator_reap_expired(server->allocator, dg_now_monotonic_ms());

    server->tick_count++;

    return 0;
}

void dg_server_request_stop(DgServer *server) {
    if (server == NULL) {
        return;
    }
    server->running = 0;
}

int dg_server_is_running(const DgServer *server) {
    if (server == NULL) {
        return 0;
    }
    return server->running != 0;
}

void dg_server_shutdown(DgServer *server) {
    if (server == NULL) {
        return;
    }

    server->running = 0;
    dg_allocator_cleanup(server->allocator);
    server->allocator = NULL;
    dg_hotplug_shutdown();
    dg_dhcp_proto_shutdown();
    dg_log(DG_LOG_INFO, "server shutdown complete");
}