/* Exercise protocol handlers with a fake sender, without raw sockets or hardware. */
#define sendto test_sendto
#include "../src/dhcp_proto.c"
#undef sendto

#include <assert.h>

static int send_fails;
static uint8_t sent_type;
static DgInterfaceEntry iface = {.ifindex = 1, .sock_fd = 42, .usb_path = "1-2:1.0"};

const DgInterfaceEntry *dg_interface_by_ifindex(int ifindex) {
    return ifindex == iface.ifindex ? &iface : NULL;
}

ssize_t test_sendto(int fd, const void *buffer, size_t length, int flags,
                    const struct sockaddr *address, socklen_t address_length) {
    const uint8_t *frame = buffer;
    (void)flags;
    (void)address;
    (void)address_length;
    assert(fd == iface.sock_fd);
    assert(length > 270);
    sent_type = frame[270];
    return send_fails ? -1 : (ssize_t)length;
}

static size_t active_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
        if (dg_allocator_active_lease(g_state.allocator, i)) count++;
    }
    return count;
}

int main(void) {
    DgDhcpRequest request = {.ifindex = 1, .chaddr = {2,0,0,0,0,1},
                             .client_mac = "02:00:00:00:00:01",
                             .client_id = {255, 42}, .client_id_len = 2};
    DgAllocator *allocator;
    uint64_t revision;
    uint8_t frame[280] = {0};
    struct sockaddr_ll address = {.sll_ifindex = 1};
    DgDhcpRequest parsed;
    assert(inet_pton(AF_INET, "192.168.8.100", &g_state.cfg.pool.start) == 1);
    assert(inet_pton(AF_INET, "192.168.8.200", &g_state.cfg.pool.end) == 1);
    assert(inet_pton(AF_INET, "192.168.8.1", &g_state.cfg.server_ip) == 1);
    g_state.cfg.pool.lease_seconds = 60;
    g_state.cfg.static_lease_count = 1;
    strcpy(g_state.cfg.static_leases[0].usb_path, iface.usb_path);
    assert(inet_pton(AF_INET, "192.168.8.10", &g_state.cfg.static_leases[0].ip) == 1);
    assert(dg_allocator_init(&allocator, &g_state.cfg) == 0);
    g_state.allocator = allocator;

    /* Option 61 is captured verbatim by the packet parser. */
    frame[0] = 0x45;
    dg_write_be16(frame + 2, sizeof(frame));
    dg_write_be16(frame + 24, sizeof(frame) - 20);
    frame[268] = 53; frame[269] = 1; frame[270] = 3;
    frame[271] = 61; frame[272] = 2; frame[273] = 255; frame[274] = 42;
    frame[275] = 255;
    assert(dg_parse_dhcp_request(frame, sizeof(frame), &address, &parsed) == 1);
    assert(parsed.client_id_len == 2 && parsed.client_id[0] == 255 && parsed.client_id[1] == 42);

    for (int dynamic = 0; dynamic < 2; dynamic++) {
        if (dynamic) iface.usb_path[0] = '\0';
        request.has_requested_ip = 0;
        request.has_server_id = 0;
        send_fails = 0;
        assert(dg_handle_discover(&request, 1000) == 0);
        assert(sent_type == DG_DHCP_TYPE_OFFER && active_count() == 0);
        send_fails = 1;
        assert(dg_handle_request(&request, 1000) == 0);
        assert(active_count() == 0);
        send_fails = 0;
        request.has_requested_ip = 1;
        request.requested_ip = g_state.cfg.server_ip;
        assert(dg_handle_request(&request, 1000) == 0);
        assert(sent_type == DG_DHCP_TYPE_NAK && active_count() == 0);
        request.has_requested_ip = 0;
        assert(dg_handle_request(&request, 1000) == 0);
        assert(sent_type == DG_DHCP_TYPE_ACK && active_count() == 1);
        assert(dg_allocator_active_lease(allocator, 0)->client_id_len == 2);
        revision = dg_allocator_lease_revision(allocator);
        assert(dg_handle_request(&request, 2000) == 0);
        assert(active_count() == 1 && dg_allocator_lease_revision(allocator) > revision);
        assert(dg_allocator_active_lease(allocator, 0)->expires_at_ms == 62000);
        request.has_server_id = 1;
        request.server_id.s_addr = 0;
        assert(dg_handle_release(&request) == 0 && active_count() == 1);
        request.has_server_id = 0;
        assert(dg_handle_release(&request) == 0 && active_count() == 0);
    }
    dg_allocator_cleanup(allocator);
    puts("protocol lease tests passed");
    return 0;
}
