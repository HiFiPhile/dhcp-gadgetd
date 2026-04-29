#include "hotplug.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

typedef struct {
    int fd;
    int ready;
} DgHotplugState;

static DgHotplugState g_hotplug = {-1, 0};

static int dg_msg_has_field(const char *buf, ssize_t len, const char *needle) {
    ssize_t i = 0;
    size_t needle_len = strlen(needle);

    while (i < len) {
        const char *s = buf + i;
        size_t slen = strnlen(s, (size_t)(len - i));
        if (slen == needle_len && memcmp(s, needle, needle_len) == 0) {
            return 1;
        }
        i += (ssize_t)slen + 1;
    }
    return 0;
}

static int dg_msg_get_field(const char *buf, ssize_t len, const char *prefix, char *out, size_t out_size) {
    ssize_t i = 0;
    size_t prefix_len = strlen(prefix);

    if (out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    while (i < len) {
        const char *s = buf + i;
        size_t slen = strnlen(s, (size_t)(len - i));
        if (slen > prefix_len && memcmp(s, prefix, prefix_len) == 0) {
            const char *value = s + prefix_len;
            size_t value_len = slen - prefix_len;
            if (value_len + 1U > out_size) {
                return -1;
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return 0;
        }
        i += (ssize_t)slen + 1;
    }

    return -1;
}

int dg_hotplug_init(void) {
    struct sockaddr_nl addr;
    int fd;
    int flags;

    memset(&g_hotplug, 0, sizeof(g_hotplug));
    g_hotplug.fd = -1;

    fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        dg_log(DG_LOG_WARN, "hotplug monitor disabled: cannot open netlink socket");
        g_hotplug.ready = 1;
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = (uint32_t)getpid();
    addr.nl_groups = 1U;

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        dg_log(DG_LOG_WARN, "hotplug monitor disabled: cannot bind netlink socket");
        g_hotplug.ready = 1;
        return 0;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    g_hotplug.fd = fd;
    g_hotplug.ready = 1;
    dg_log(DG_LOG_INFO, "hotplug module initialized");
    return 0;
}

int dg_hotplug_fd(void) {
    return g_hotplug.fd;
}

int dg_hotplug_next_event(DgHotplugEvent *out_event) {
    char buf[4096];
    ssize_t nread;

    if (out_event == NULL) {
        return -1;
    }
    if (!g_hotplug.ready) {
        return -1;
    }
    if (g_hotplug.fd < 0) {
        return 0;
    }

    while (1) {
        char action[16];
        char ifname[16];
        char ifindex_text[32];
        unsigned long ifindex_ul = 0;
        char *endptr = NULL;

        nread = recv(g_hotplug.fd, buf, sizeof(buf), 0);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            return -1;
        }
        if (nread == 0) {
            break;
        }

        if (!dg_msg_has_field(buf, nread, "SUBSYSTEM=net")) {
            continue;
        }

        if (dg_msg_get_field(buf, nread, "ACTION=", action, sizeof(action)) != 0 ||
            dg_msg_get_field(buf, nread, "INTERFACE=", ifname, sizeof(ifname)) != 0) {
            continue;
        }

        memset(out_event, 0, sizeof(*out_event));
        if (snprintf(out_event->ifname, sizeof(out_event->ifname), "%s", ifname) < 0) {
            continue;
        }

        if (dg_msg_get_field(buf, nread, "IFINDEX=", ifindex_text, sizeof(ifindex_text)) == 0) {
            errno = 0;
            ifindex_ul = strtoul(ifindex_text, &endptr, 10);
            if (errno == 0 && endptr != ifindex_text && *endptr == '\0' && ifindex_ul <= (unsigned long)INT32_MAX) {
                out_event->ifindex = (int)ifindex_ul;
            }
        }

        if (strcmp(action, "add") == 0) {
            out_event->action = DG_HOTPLUG_ADD;
            return 1;
        }
        if (strcmp(action, "move") == 0) {
            out_event->action = DG_HOTPLUG_MOVE;
            return 1;
        }
        if (strcmp(action, "remove") == 0) {
            out_event->action = DG_HOTPLUG_REMOVE;
            return 1;
        }
    }

    return 0;
}

void dg_hotplug_shutdown(void) {
    if (g_hotplug.fd >= 0) {
        close(g_hotplug.fd);
    }
    g_hotplug.fd = -1;
    g_hotplug.ready = 0;
    dg_log(DG_LOG_INFO, "hotplug module shutdown");
}