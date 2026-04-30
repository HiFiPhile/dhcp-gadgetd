#include "server.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "dhcp_proto.h"
#include "hotplug.h"
#include "interface.h"
#include "log.h"

#define DG_SERVER_MAX_POLL_FDS 129U

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

    if (dg_interface_init() != 0) {
        dg_log(DG_LOG_WARN, "initial interface scan returned error");
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
    struct pollfd pfds[DG_SERVER_MAX_POLL_FDS];
    size_t iface_map[DG_SERVER_MAX_POLL_FDS];
    int poll_rc;
    int hotplug_ready = 0;
    nfds_t nfds = 0U;
    size_t active_iface_count = 0U;
    size_t hotplug_idx = 0U;
    size_t i;

    if (server == NULL || !server->running) {
        return -1;
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    for (i = 0; i < dg_interface_count() && nfds + 1U < DG_SERVER_MAX_POLL_FDS; i++) {
        const DgInterfaceEntry *iface = dg_interface_get(i);
        if (iface == NULL || iface->sock_fd < 0 || !dg_interface_is_up(iface)) {
            continue;
        }

        iface_map[active_iface_count] = i;
        pfds[nfds].fd = iface->sock_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
        active_iface_count++;
    }

    if (dg_hotplug_fd() >= 0 && nfds < DG_SERVER_MAX_POLL_FDS) {
        hotplug_idx = (size_t)nfds;
        pfds[nfds].fd = dg_hotplug_fd();
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }

    if (nfds == 0U) {
        (void)poll(NULL, 0, timeout_ms);
    } else {
        poll_rc = poll(pfds, nfds, timeout_ms);
        if (poll_rc < 0) {
            if (errno != EINTR) {
                dg_log(DG_LOG_ERROR, "poll failed in server tick");
                return -1;
            }
        } else if (poll_rc > 0) {
            if (dg_hotplug_fd() >= 0 && (pfds[hotplug_idx].revents & POLLIN) != 0) {
                hotplug_ready = 1;
            }

            for (i = 0; i < active_iface_count; i++) {
                const DgInterfaceEntry *iface = dg_interface_get(iface_map[i]);
                if (iface == NULL) {
                    continue;
                }

                if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    int error = 0;
                    socklen_t errlen = sizeof(error);
                    (void)getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
                    dg_log(DG_LOG_WARN, "interface socket error if=%s idx=%d err=%d", iface->ifname,
                           iface->ifindex, error);
                    continue;
                }

                if ((pfds[i].revents & POLLIN) != 0) {
                    if (dg_dhcp_proto_drain_socket(iface, dg_now_monotonic_ms()) != 0) {
                        dg_log(DG_LOG_WARN, "interface socket recv error if=%s idx=%d", iface->ifname,
                               iface->ifindex);
                    }
                }
            }
        }
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

            dg_log(DG_LOG_DEBUG, "hotplug event: action=%s if=%s idx=%d driver=%s devpath=%s",
                dg_hotplug_action_name(evt.action), evt.ifname, evt.ifindex, evt.id_net_driver, evt.devpath);

            if (evt.action == DG_HOTPLUG_REMOVE) {
                (void)dg_interface_remove(&evt);
                continue;
            }

            if (evt.action == DG_HOTPLUG_ADD || evt.action == DG_HOTPLUG_MOVE) {
                if (dg_interface_upsert(&evt) == 0) {
                    dg_log(DG_LOG_DEBUG, "hotplug upserted interface: if=%s idx=%d", evt.ifname, evt.ifindex);
                } else {
                    dg_log(DG_LOG_DEBUG,
                           "skip hotplug add/move: interface upsert rejected if=%s idx=%d driver=%s devpath=%s",
                           evt.ifname, evt.ifindex, evt.id_net_driver, evt.devpath);
                }
            }
        }
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