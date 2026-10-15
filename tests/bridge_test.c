/* Needs a throwaway network namespace: unshare -rn ./tests/bridge_test */
#include "../src/bridge.c"

#include <assert.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>

typedef struct {
    int up;
    uint8_t mac[6];
} LinkState;

typedef struct {
    int ifindex;
    size_t count;
    struct in_addr local;
    uint8_t prefix;
    struct in_addr broadcast; /* 0.0.0.0 when the kernel has none */
} AddrState;

static LinkState link_state(const char *name) {
    struct ifaddrs *list;
    LinkState state;

    memset(&state, 0, sizeof(state));
    assert(getifaddrs(&list) == 0);
    for (struct ifaddrs *it = list; it; it = it->ifa_next) {
        if (strcmp(it->ifa_name, name) != 0 || !it->ifa_addr || it->ifa_addr->sa_family != AF_PACKET) continue;
        state.up = (it->ifa_flags & IFF_UP) != 0;
        memcpy(state.mac, ((const struct sockaddr_ll *)it->ifa_addr)->sll_addr, 6);
    }
    freeifaddrs(list);
    return state;
}

/* Reads addresses over netlink: getifaddrs() reports a missing broadcast as the address itself. */
static int on_addr(const struct nlmsghdr *msg, void *userdata) {
    AddrState *state = userdata;
    const struct ifaddrmsg *ifa = NLMSG_DATA(msg);
    const struct rtattr *rta;
    int len = (int)IFA_PAYLOAD(msg);

    if (ifa->ifa_family != AF_INET || (int)ifa->ifa_index != state->ifindex) return 0;
    state->count++;
    state->prefix = ifa->ifa_prefixlen;
    for (rta = IFA_RTA(ifa); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFA_LOCAL) memcpy(&state->local, RTA_DATA(rta), 4);
        if (rta->rta_type == IFA_BROADCAST) memcpy(&state->broadcast, RTA_DATA(rta), 4);
    }
    return 0;
}

static AddrState addr_state(int ifindex) {
    DgNlRequest req;
    AddrState state = {.ifindex = ifindex};
    struct ifaddrmsg *ifa = dg_nl_init(&req, RTM_GETADDR, NLM_F_DUMP, sizeof(*ifa));

    ifa->ifa_family = AF_INET;
    assert(dg_nl_transact(&req, on_addr, &state) == 0);
    return state;
}

static int on_master(const struct nlmsghdr *msg, void *userdata) {
    const struct ifinfomsg *ifi = NLMSG_DATA(msg);
    const struct rtattr *rta;
    int len = (int)IFLA_PAYLOAD(msg);

    for (rta = IFLA_RTA(ifi); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFLA_MASTER) memcpy(userdata, RTA_DATA(rta), sizeof(int));
    }
    return 0;
}

static int link_master(int ifindex) {
    DgNlRequest req;
    struct ifinfomsg *ifi = dg_nl_init(&req, RTM_GETLINK, 0, sizeof(*ifi));
    int master = 0;

    ifi->ifi_index = ifindex;
    assert(dg_nl_transact(&req, on_master, &master) == 0);
    return master;
}

static int new_dummy(const char *name) {
    DgLinkInfo link;

    assert(dg_nl_new_link(name, "dummy") == 0);
    assert(dg_nl_get_link(name, &link) == 0 && !link.is_bridge);
    return link.ifindex;
}

static void set_subnet(DgConfig *cfg, const char *server, const char *mask, uint8_t prefix) {
    assert(inet_pton(AF_INET, server, &cfg->server_ip) == 1);
    assert(inet_pton(AF_INET, mask, &cfg->subnet_mask) == 1);
    cfg->subnet_prefix = prefix;
}

static void check_address(int ifindex, const char *local, uint8_t prefix, const char *broadcast) {
    AddrState state = addr_state(ifindex);
    struct in_addr expected;

    assert(state.count == 1 && state.prefix == prefix);
    assert(inet_pton(AF_INET, local, &expected) == 1 && state.local.s_addr == expected.s_addr);
    assert(inet_pton(AF_INET, broadcast, &expected) == 1 && state.broadcast.s_addr == expected.s_addr);
}

int main(void) {
    DgConfig cfg = {0};
    DgNlRequest req;
    DgLinkInfo bridge;
    LinkState state;
    uint8_t mac[6];
    struct in_addr stale;
    int p0;
    int p1;

    /* An existing interface of another kind is never adopted. */
    strcpy(cfg.bridge_name, "lo");
    set_subnet(&cfg, "192.168.8.1", "255.255.255.0", 24);
    assert(dg_bridge_init(&cfg) != 0);

    strcpy(cfg.bridge_name, "gadget0");
    assert(dg_bridge_init(&cfg) == 0);
    assert(dg_nl_get_link("gadget0", &bridge) == 0 && bridge.is_bridge);
    assert(dg_bridge_mac(mac) == 0);
    assert((mac[0] & 0x03U) == 0x02U);
    state = link_state("gadget0");
    assert(state.up && memcmp(state.mac, mac, 6) == 0);
    check_address(bridge.ifindex, "192.168.8.1", 24, "192.168.8.255");

    /* Ports join the bridge and come up, without the bridge taking their MAC. */
    p0 = new_dummy("p0");
    p1 = new_dummy("p1");
    assert(dg_bridge_add_port(p0, "p0") == 0);
    assert(dg_bridge_add_port(p0, "p0") == 0);
    assert(link_master(p0) == bridge.ifindex && link_state("p0").up);
    assert(memcmp(link_state("gadget0").mac, mac, 6) == 0);
    assert(dg_bridge_add_port(99999, "missing") != 0);

    /* Restart adopts the bridge, keeps its ports and drops addresses it did not configure. */
    assert(inet_pton(AF_INET, "10.9.0.1", &stale) == 1);
    assert(dg_nl_init_addr(&req, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, stale, 16) == 0);
    assert(dg_nl_transact(&req, NULL, NULL) == 0);
    assert(addr_state(bridge.ifindex).count == 2);
    dg_bridge_shutdown();
    assert(dg_bridge_init(&cfg) == 0);
    assert(g_bridge.ifindex == bridge.ifindex && link_master(p0) == bridge.ifindex);
    check_address(bridge.ifindex, "192.168.8.1", 24, "192.168.8.255");

    /* A changed subnet replaces the old address. */
    dg_bridge_shutdown();
    set_subnet(&cfg, "10.1.0.1", "255.255.0.0", 16);
    assert(dg_bridge_init(&cfg) == 0);
    check_address(bridge.ifindex, "10.1.0.1", 16, "10.1.255.255");
    assert(dg_bridge_add_port(p1, "p1") == 0 && link_master(p1) == bridge.ifindex);

    /* /31 links have no broadcast address. */
    dg_bridge_shutdown();
    set_subnet(&cfg, "10.2.0.0", "255.255.255.254", 31);
    assert(dg_bridge_init(&cfg) == 0);
    check_address(bridge.ifindex, "10.2.0.0", 31, "0.0.0.0");

    dg_bridge_shutdown();
    puts("bridge tests passed");
    return 0;
}
