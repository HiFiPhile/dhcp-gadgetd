#include "bridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <systemd/sd-id128.h>
#include <unistd.h>

#include "log.h"

#define DG_BRIDGE_MAX_ADDRS 32U

/* Salt for deriving the bridge MAC from /etc/machine-id; changing it changes every host's MAC. */
#define DG_BRIDGE_MAC_APP_ID \
    SD_ID128_MAKE(a5, 08, 14, 5c, 2a, 4d, 42, 55, 88, 52, 57, a0, fa, c4, 98, 62)

typedef union {
    struct nlmsghdr hdr;
    char buf[512];
} DgNlRequest;

typedef int (*DgNlHandler)(const struct nlmsghdr *msg, void *userdata);

typedef struct {
    int ifindex;
    int is_bridge;
} DgLinkInfo;

typedef struct {
    int ifindex;
    size_t count;
    struct {
        struct in_addr local;
        uint8_t prefix;
    } addrs[DG_BRIDGE_MAX_ADDRS];
} DgAddrList;

typedef struct {
    int fd;
    uint32_t seq;
    int ifindex;
    char name[DG_MAX_IFNAME_LEN];
} DgBridgeState;

static DgBridgeState g_bridge = {-1, 0, 0, ""};

static void *dg_nl_init(DgNlRequest *req, uint16_t type, uint16_t flags, size_t header_len) {
    memset(req, 0, sizeof(*req));
    req->hdr.nlmsg_len = (uint32_t)NLMSG_LENGTH(header_len);
    req->hdr.nlmsg_type = type;
    req->hdr.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_ACK | flags);
    return NLMSG_DATA(&req->hdr);
}

static struct rtattr *dg_nl_put(DgNlRequest *req, unsigned short type, const void *data, size_t len) {
    size_t off = NLMSG_ALIGN(req->hdr.nlmsg_len);
    struct rtattr *rta;

    if (off + RTA_SPACE(len) > sizeof(req->buf)) {
        return NULL;
    }

    rta = (struct rtattr *)(req->buf + off);
    rta->rta_type = type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    if (len > 0U) {
        memcpy(RTA_DATA(rta), data, len);
    }
    req->hdr.nlmsg_len = (uint32_t)(off + RTA_SPACE(len));
    return rta;
}

static void dg_nl_end_nest(DgNlRequest *req, struct rtattr *nest) {
    nest->rta_len = (unsigned short)(req->buf + req->hdr.nlmsg_len - (char *)nest);
}

/* Sends one request and reads until its ACK or dump end; returns 0 or a negative errno. */
static int dg_nl_transact(DgNlRequest *req, DgNlHandler handler, void *userdata) {
    union {
        struct nlmsghdr hdr;
        char buf[32768];
    } resp;
    uint32_t seq = ++g_bridge.seq;
    int result = 0;

    req->hdr.nlmsg_seq = seq;
    if (send(g_bridge.fd, req->buf, req->hdr.nlmsg_len, 0) < 0) {
        return -errno;
    }

    while (1) {
        const struct nlmsghdr *msg;
        ssize_t len = recv(g_bridge.fd, resp.buf, sizeof(resp.buf), 0);

        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }

        for (msg = &resp.hdr; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len)) {
            if (msg->nlmsg_seq != seq) {
                continue;
            }
            if (msg->nlmsg_type == NLMSG_DONE) {
                const int *error = NLMSG_DATA(msg);
                if (msg->nlmsg_len >= NLMSG_LENGTH(sizeof(*error)) && *error < 0) {
                    return *error;
                }
                return result;
            }
            if (msg->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *err = NLMSG_DATA(msg);
                if (msg->nlmsg_len < NLMSG_LENGTH(sizeof(*err))) {
                    return -EBADMSG;
                }
                return err->error != 0 ? err->error : result;
            }
            /* Keep reading after a handler error so the socket stays in sync. */
            if (handler != NULL && result == 0) {
                result = handler(msg, userdata);
            }
        }
    }
}

static int dg_on_link(const struct nlmsghdr *msg, void *userdata) {
    DgLinkInfo *link = userdata;
    const struct ifinfomsg *ifi = NLMSG_DATA(msg);
    const struct rtattr *rta;
    int len;

    if (msg->nlmsg_type != RTM_NEWLINK || msg->nlmsg_len < NLMSG_LENGTH(sizeof(*ifi))) {
        return 0;
    }

    link->ifindex = ifi->ifi_index;
    len = (int)IFLA_PAYLOAD(msg);
    for (rta = IFLA_RTA(ifi); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        const struct rtattr *info;
        int info_len;

        if (rta->rta_type != IFLA_LINKINFO) {
            continue;
        }
        info_len = (int)RTA_PAYLOAD(rta);
        for (info = RTA_DATA(rta); RTA_OK(info, info_len); info = RTA_NEXT(info, info_len)) {
            /* The kernel sends the kind NUL-terminated. */
            if (info->rta_type == IFLA_INFO_KIND) {
                link->is_bridge = RTA_PAYLOAD(info) == sizeof("bridge") &&
                                  memcmp(RTA_DATA(info), "bridge", sizeof("bridge")) == 0;
            }
        }
    }
    return 0;
}

static int dg_nl_get_link(const char *name, DgLinkInfo *out) {
    DgNlRequest req;

    memset(out, 0, sizeof(*out));
    (void)dg_nl_init(&req, RTM_GETLINK, 0, sizeof(struct ifinfomsg));
    if (dg_nl_put(&req, IFLA_IFNAME, name, strlen(name) + 1U) == NULL) {
        return -EMSGSIZE;
    }
    return dg_nl_transact(&req, dg_on_link, out);
}

static int dg_nl_new_link(const char *name, const char *kind) {
    DgNlRequest req;
    struct rtattr *info;

    (void)dg_nl_init(&req, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifinfomsg));
    if (dg_nl_put(&req, IFLA_IFNAME, name, strlen(name) + 1U) == NULL) {
        return -EMSGSIZE;
    }
    info = dg_nl_put(&req, IFLA_LINKINFO, NULL, 0U);
    if (info == NULL || dg_nl_put(&req, IFLA_INFO_KIND, kind, strlen(kind)) == NULL) {
        return -EMSGSIZE;
    }
    dg_nl_end_nest(&req, info);
    return dg_nl_transact(&req, NULL, NULL);
}

/* Sets one link attribute (MAC, master) and IFF_UP in a single request. */
static int dg_nl_set_link_up(int ifindex, unsigned short type, const void *data, size_t len) {
    DgNlRequest req;
    struct ifinfomsg *ifi = dg_nl_init(&req, RTM_SETLINK, 0, sizeof(*ifi));

    ifi->ifi_index = ifindex;
    ifi->ifi_flags = IFF_UP;
    ifi->ifi_change = IFF_UP;
    if (dg_nl_put(&req, type, data, len) == NULL) {
        return -EMSGSIZE;
    }
    return dg_nl_transact(&req, NULL, NULL);
}

static int dg_nl_init_addr(DgNlRequest *req, uint16_t type, uint16_t flags, struct in_addr local, uint8_t prefix) {
    struct ifaddrmsg *ifa = dg_nl_init(req, type, flags, sizeof(*ifa));

    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = prefix;
    ifa->ifa_index = (unsigned int)g_bridge.ifindex;
    return dg_nl_put(req, IFA_LOCAL, &local, sizeof(local)) != NULL ? 0 : -EMSGSIZE;
}

static int dg_on_addr(const struct nlmsghdr *msg, void *userdata) {
    DgAddrList *list = userdata;
    const struct ifaddrmsg *ifa = NLMSG_DATA(msg);
    const struct rtattr *rta;
    int len;

    if (msg->nlmsg_type != RTM_NEWADDR || msg->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa)) ||
        ifa->ifa_family != AF_INET || (int)ifa->ifa_index != list->ifindex) {
        return 0;
    }

    len = (int)IFA_PAYLOAD(msg);
    for (rta = IFA_RTA(ifa); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type != IFA_LOCAL || RTA_PAYLOAD(rta) != sizeof(struct in_addr)) {
            continue;
        }
        if (list->count >= DG_BRIDGE_MAX_ADDRS) {
            return -E2BIG;
        }
        memcpy(&list->addrs[list->count].local, RTA_DATA(rta), sizeof(struct in_addr));
        list->addrs[list->count].prefix = ifa->ifa_prefixlen;
        list->count++;
    }
    return 0;
}

static int dg_bridge_sync_addresses(const DgConfig *cfg) {
    DgNlRequest req;
    DgAddrList list;
    struct ifaddrmsg *ifa;
    size_t i;
    int r;

    memset(&list, 0, sizeof(list));
    list.ifindex = g_bridge.ifindex;
    ifa = dg_nl_init(&req, RTM_GETADDR, NLM_F_DUMP, sizeof(*ifa));
    ifa->ifa_family = AF_INET;
    r = dg_nl_transact(&req, dg_on_addr, &list);
    if (r < 0) {
        return r;
    }

    for (i = 0; i < list.count; i++) {
        char ip_text[INET_ADDRSTRLEN];

        if (list.addrs[i].local.s_addr == cfg->server_ip.s_addr && list.addrs[i].prefix == cfg->subnet_prefix) {
            continue;
        }
        r = dg_nl_init_addr(&req, RTM_DELADDR, 0, list.addrs[i].local, list.addrs[i].prefix);
        if (r == 0) {
            r = dg_nl_transact(&req, NULL, NULL);
        }
        /* Deleting a primary address also deletes its secondaries, so they may already be gone. */
        if (r < 0 && r != -EADDRNOTAVAIL) {
            return r;
        }
        if (inet_ntop(AF_INET, &list.addrs[i].local, ip_text, sizeof(ip_text)) != NULL) {
            dg_log(DG_LOG_INFO, "bridge stale address removed name=%s address=%s/%u", g_bridge.name, ip_text,
                   list.addrs[i].prefix);
        }
    }

    r = dg_nl_init_addr(&req, RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE, cfg->server_ip, cfg->subnet_prefix);
    /* /31 and /32 have no broadcast address (RFC 3021). */
    if (r == 0 && cfg->subnet_prefix <= 30U) {
        struct in_addr broadcast;
        broadcast.s_addr = cfg->server_ip.s_addr | ~cfg->subnet_mask.s_addr;
        if (dg_nl_put(&req, IFA_BROADCAST, &broadcast, sizeof(broadcast)) == NULL) {
            r = -EMSGSIZE;
        }
    }
    if (r == 0) {
        r = dg_nl_transact(&req, NULL, NULL);
    }
    return r;
}

/*
 * Without an explicit MAC the bridge copies the lowest port MAC, which changes as
 * gadgets come and go and leaves clients with stale ARP entries.
 */
static int dg_bridge_mac(uint8_t mac[6]) {
    sd_id128_t id;
    int r = sd_id128_get_machine_app_specific(DG_BRIDGE_MAC_APP_ID, &id);

    if (r < 0) {
        return r;
    }
    memcpy(mac, id.bytes, 6U);
    mac[0] = (uint8_t)((mac[0] & 0xFEU) | 0x02U); /* unicast, locally administered */
    return 0;
}

int dg_bridge_init(const DgConfig *cfg) {
    DgLinkInfo link;
    uint8_t mac[6];
    char ip_text[INET_ADDRSTRLEN];
    const char *action = "adopted";
    int r;

    g_bridge.fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (g_bridge.fd < 0) {
        dg_log(DG_LOG_ERROR, "bridge setup failed: cannot open rtnetlink socket: %s", strerror(errno));
        return -1;
    }
    (void)snprintf(g_bridge.name, sizeof(g_bridge.name), "%s", cfg->bridge_name);

    r = dg_nl_get_link(g_bridge.name, &link);
    if (r == -ENODEV) {
        action = "created";
        r = dg_nl_new_link(g_bridge.name, "bridge");
        if (r == 0) {
            r = dg_nl_get_link(g_bridge.name, &link);
        }
    }
    if (r == 0 && !link.is_bridge) {
        dg_log(DG_LOG_ERROR, "bridge setup failed name=%s: interface exists and is not a bridge", g_bridge.name);
        dg_bridge_shutdown();
        return -1;
    }

    if (r == 0) {
        g_bridge.ifindex = link.ifindex;
        r = dg_bridge_mac(mac);
    }
    if (r == 0) {
        r = dg_nl_set_link_up(g_bridge.ifindex, IFLA_ADDRESS, mac, sizeof(mac));
    }
    if (r == 0) {
        r = dg_bridge_sync_addresses(cfg);
    }
    if (r < 0) {
        dg_log(DG_LOG_ERROR, "bridge setup failed name=%s: %s", g_bridge.name, strerror(-r));
        dg_bridge_shutdown();
        return -1;
    }

    if (inet_ntop(AF_INET, &cfg->server_ip, ip_text, sizeof(ip_text)) == NULL) {
        strcpy(ip_text, "<invalid>");
    }
    dg_log(DG_LOG_INFO, "bridge %s name=%s idx=%d mac=%02x:%02x:%02x:%02x:%02x:%02x address=%s/%u", action,
           g_bridge.name, g_bridge.ifindex, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ip_text,
           cfg->subnet_prefix);
    return 0;
}

int dg_bridge_add_port(int ifindex, const char *ifname) {
    uint32_t master = (uint32_t)g_bridge.ifindex;
    int r = dg_nl_set_link_up(ifindex, IFLA_MASTER, &master, sizeof(master));

    if (r < 0) {
        dg_log(DG_LOG_WARN, "bridge port add failed if=%s idx=%d bridge=%s err=%s", ifname, ifindex, g_bridge.name,
               strerror(-r));
        return -1;
    }
    dg_log(DG_LOG_INFO, "bridge port added if=%s idx=%d bridge=%s", ifname, ifindex, g_bridge.name);
    return 0;
}

void dg_bridge_shutdown(void) {
    if (g_bridge.fd >= 0) {
        close(g_bridge.fd);
    }
    g_bridge.fd = -1;
    g_bridge.ifindex = 0;
    dg_log(DG_LOG_INFO, "bridge module shutdown (bridge kept)");
}
