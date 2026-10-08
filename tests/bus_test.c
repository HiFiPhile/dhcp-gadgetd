#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bus.h"

static sd_bus *client;
static DgBus *server;
static sd_bus_message *response;
static unsigned int changes;

static uint64_t now_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int on_reply(sd_bus_message *message, void *userdata, sd_bus_error *error) {
    (void)userdata;
    (void)error;
    response = sd_bus_message_ref(message);
    return 1;
}

static int on_change(sd_bus_message *message, void *userdata, sd_bus_error *error) {
    const char *interface;
    (void)userdata;
    (void)error;
    assert(sd_bus_message_read(message, "s", &interface) > 0);
    assert(strcmp(interface, DG_BUS_INTERFACE) == 0);
    changes++;
    return 0;
}

static void pump(void) {
    assert(dg_bus_process(server) >= 0);
    while (sd_bus_process(client, NULL) > 0) {}
}

static sd_bus_message *call(const char *interface, const char *method,
                            const char *property) {
    sd_bus_message *request = NULL;
    sd_bus_slot *slot = NULL;
    uint64_t deadline = now_ms() + 3000;
    response = NULL;
    assert(sd_bus_message_new_method_call(client, &request, DG_BUS_NAME, DG_BUS_PATH,
                                          interface, method) >= 0);
    if (property) {
        assert(sd_bus_message_append(request, "ss", DG_BUS_INTERFACE, property) >= 0);
    }
    if (strcmp(method, "Set") == 0) {
        assert(sd_bus_message_open_container(request, 'v', "u") >= 0);
        assert(sd_bus_message_append(request, "u", (uint32_t)1) >= 0);
        assert(sd_bus_message_close_container(request) >= 0);
    }
    assert(sd_bus_call_async(client, &slot, request, on_reply, NULL, 2000000) >= 0);
    sd_bus_message_unref(request);
    while (!response && now_ms() < deadline) {
        pump();
        assert(sd_bus_wait(client, 1000) >= 0);
    }
    assert(response);
    sd_bus_slot_unref(slot);
    return response;
}

static sd_bus_message *property(const char *name, const char *signature) {
    sd_bus_message *reply = call("org.freedesktop.DBus.Properties", "Get", name);
    assert(!sd_bus_message_is_method_error(reply, NULL));
    assert(sd_bus_message_enter_container(reply, 'v', signature) > 0);
    return reply;
}

static void check_bytes(sd_bus_message *reply, const void *expected, size_t size) {
    const void *bytes;
    size_t length;
    assert(sd_bus_message_read_array(reply, 'y', &bytes, &length) > 0);
    assert(length == size);
    if (size) assert(memcmp(bytes, expected, size) == 0);
}

static void check_lease(struct in_addr ip, const uint8_t *mac, const uint8_t *id,
                         size_t id_len, uint64_t expiration) {
    sd_bus_message *reply = property("Leases", "a(uayayayayt)");
    uint32_t family;
    uint64_t actual_expiration;
    const uint8_t gateway[] = {192, 168, 8, 1};
    assert(sd_bus_message_enter_container(reply, 'a', "(uayayayayt)") > 0);
    assert(sd_bus_message_enter_container(reply, 'r', "uayayayayt") > 0);
    assert(sd_bus_message_read(reply, "u", &family) > 0 && family == AF_INET);
    check_bytes(reply, id, id_len);
    check_bytes(reply, &ip, 4);
    check_bytes(reply, gateway, 4);
    check_bytes(reply, mac, 6);
    assert(sd_bus_message_read(reply, "t", &actual_expiration) > 0);
    assert(actual_expiration == expiration * 1000);
    assert(sd_bus_message_exit_container(reply) > 0);
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
}

static void check_empty(void) {
    sd_bus_message *reply = property("Leases", "a(uayayayayt)");
    assert(sd_bus_message_enter_container(reply, 'a', "(uayayayayt)") > 0);
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
}

static void check_options(int optional) {
    sd_bus_message *reply = property("Options", "a(yay)");
    const uint8_t codes[] = {54, 1, 51, 3, 6, 224};
    const uint8_t values[][4] = {{192,168,8,1}, {255,255,255,0}, {0,0,0,60},
                                  {192,168,8,1}, {1,1,1,1}, {1,2,255,0}};
    assert(sd_bus_message_enter_container(reply, 'a', "(yay)") > 0);
    for (size_t i = 0; i < (optional ? 6U : 3U); i++) {
        uint8_t code;
        assert(sd_bus_message_enter_container(reply, 'r', "yay") > 0);
        assert(sd_bus_message_read(reply, "y", &code) > 0 && code == codes[i]);
        check_bytes(reply, values[i], i == 5 ? 3 : 4);
        assert(sd_bus_message_exit_container(reply) > 0);
    }
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
}

static void check_config(const DgConfig *cfg) {
    const struct { const char *name; const struct in_addr *address; } addresses[] = {
        {"ServerAddress", &cfg->server_ip}, {"SubnetAddress", &cfg->subnet_network},
        {"SubnetMask", &cfg->subnet_mask}, {"PoolStart", &cfg->pool.start},
        {"PoolEnd", &cfg->pool.end},
    };
    sd_bus_message *reply;
    uint8_t prefix;
    uint32_t seconds;
    const char *text;

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        reply = property(addresses[i].name, "ay");
        check_bytes(reply, addresses[i].address, 4);
        sd_bus_message_unref(reply);
    }
    reply = property("Router", "ay");
    check_bytes(reply, &cfg->router, cfg->has_router ? 4 : 0);
    sd_bus_message_unref(reply);
    reply = property("SubnetPrefixLength", "y");
    assert(sd_bus_message_read(reply, "y", &prefix) > 0 && prefix == 24);
    sd_bus_message_unref(reply);
    reply = property("LeaseTime", "u");
    assert(sd_bus_message_read(reply, "u", &seconds) > 0 && seconds == 60);
    sd_bus_message_unref(reply);
    reply = property("ConfigFile", "s");
    assert(sd_bus_message_read(reply, "s", &text) > 0 && strcmp(text, cfg->config_path) == 0);
    sd_bus_message_unref(reply);
    reply = property("DNS", "aay");
    assert(sd_bus_message_enter_container(reply, 'a', "ay") > 0);
    for (size_t i = 0; i < cfg->dns_count; i++) check_bytes(reply, &cfg->dns[i], 4);
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
    reply = property("StaticLeases", "a(say)");
    assert(sd_bus_message_enter_container(reply, 'a', "(say)") > 0);
    for (size_t i = 0; i < cfg->static_lease_count; i++) {
        assert(sd_bus_message_enter_container(reply, 'r', "say") > 0);
        assert(sd_bus_message_read(reply, "s", &text) > 0);
        assert(strcmp(text, cfg->static_leases[i].usb_path) == 0);
        check_bytes(reply, &cfg->static_leases[i].ip, 4);
        assert(sd_bus_message_exit_container(reply) > 0);
    }
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
    reply = property("CustomOptions", "a(yay)");
    assert(sd_bus_message_enter_container(reply, 'a', "(yay)") > 0);
    for (size_t i = 0; i < cfg->custom_option_count; i++) {
        uint8_t code;
        assert(sd_bus_message_enter_container(reply, 'r', "yay") > 0);
        assert(sd_bus_message_read(reply, "y", &code) > 0 && code == cfg->custom_options[i].code);
        check_bytes(reply, cfg->custom_options[i].data, cfg->custom_options[i].data_len);
        assert(sd_bus_message_exit_container(reply) > 0);
    }
    assert(sd_bus_message_at_end(reply, 0) > 0);
    sd_bus_message_unref(reply);
    reply = call("org.freedesktop.DBus.Properties", "Set", "LeaseTime");
    assert(sd_bus_message_is_method_error(reply, SD_BUS_ERROR_PROPERTY_READ_ONLY));
    sd_bus_message_unref(reply);
}

int main(void) {
    DgConfig cfg = {0};
    DgAllocator *allocator = NULL;
    sd_bus *connection = NULL;
    sd_bus_slot *match = NULL;
    sd_bus_message *reply;
    struct in_addr ip;
    uint32_t value;
    uint64_t now;
    const uint8_t mac[] = {2,0,0,0,0,1};
    const uint8_t id[] = {255, 0, 42};
    const uint8_t fallback_id[] = {1,2,0,0,0,0,1};
    struct pollfd pfd;
    int timeout = 250;
    const char *xml;

    assert(inet_pton(AF_INET, "192.168.8.0", &cfg.subnet_network) == 1);
    assert(inet_pton(AF_INET, "255.255.255.0", &cfg.subnet_mask) == 1);
    assert(inet_pton(AF_INET, "192.168.8.1", &cfg.server_ip) == 1);
    assert(inet_pton(AF_INET, "192.168.8.100", &cfg.pool.start) == 1);
    assert(inet_pton(AF_INET, "192.168.8.200", &cfg.pool.end) == 1);
    cfg.router = cfg.server_ip;
    cfg.pool.lease_seconds = 60;
    cfg.subnet_prefix = 24;
    strcpy(cfg.config_path, "/etc/dhcp-gadget/config.conf");
    assert(dg_allocator_init(&allocator, &cfg) == 0);
    assert(sd_bus_open_user(&connection) >= 0);
    assert(sd_bus_open_user(&client) >= 0);
    assert(dg_bus_init(&server, &cfg, allocator, connection) == 0);
    assert(sd_bus_match_signal(client, &match, DG_BUS_NAME, DG_BUS_PATH,
                               "org.freedesktop.DBus.Properties", "PropertiesChanged",
                               on_change, NULL) >= 0);
    assert(dg_bus_prepare_poll(server, &pfd, &timeout) == 0 && pfd.fd >= 0);
    reply = call("org.freedesktop.DBus.Introspectable", "Introspect", NULL);
    assert(sd_bus_message_read(reply, "s", &xml) > 0);
    assert(strstr(xml, "a(uayayayayt)") && strstr(xml, "a(yay)"));
    sd_bus_message_unref(reply);
    reply = property("PoolSize", "u");
    assert(sd_bus_message_read(reply, "u", &value) > 0 && value == 101);
    sd_bus_message_unref(reply);
    reply = property("PoolOffset", "u");
    assert(sd_bus_message_read(reply, "u", &value) > 0 && value == 100);
    sd_bus_message_unref(reply);
    check_options(0);
    check_config(&cfg);
    /* These fixtures change before further requests; production config is immutable. */
    cfg.has_router = 1;
    cfg.dns_count = 1;
    assert(inet_pton(AF_INET, "1.1.1.1", &cfg.dns[0]) == 1);
    cfg.custom_option_count = 1;
    cfg.custom_options[0] = (DgCustomOption){.code = 224, .data = {1,2,255}, .data_len = 3};
    check_options(1);
    cfg.dns_count = 2;
    assert(inet_pton(AF_INET, "8.8.8.8", &cfg.dns[1]) == 1);
    cfg.static_lease_count = 2;
    strcpy(cfg.static_leases[0].usb_path, "1-2:1.0");
    strcpy(cfg.static_leases[1].usb_path, "1-3:1.0");
    assert(inet_pton(AF_INET, "192.168.8.10", &cfg.static_leases[0].ip) == 1);
    assert(inet_pton(AF_INET, "192.168.8.11", &cfg.static_leases[1].ip) == 1);
    check_config(&cfg);
    check_empty();
    now = now_ms();
    assert(dg_allocator_assign_dynamic(allocator, "02:00:00:00:00:01", now, &ip) == 0);
    check_empty(); /* DISCOVER reservations must not appear as active leases. */
    assert(changes == 0);
    assert(dg_allocator_ack(allocator, mac, id, sizeof(id), ip, now) == 0);
    check_lease(ip, mac, id, sizeof(id), now + 60000);
    assert(changes == 1);
    assert(dg_allocator_ack(allocator, mac, id, sizeof(id), ip, now + 1000) == 0);
    check_lease(ip, mac, id, sizeof(id), now + 61000);
    assert(changes == 2);
    assert(dg_allocator_release_mac(allocator, "02:00:00:00:00:01") == 0);
    check_empty();
    assert(changes == 3);
    /* A static ACK needs no dynamic reservation. */
    assert(inet_pton(AF_INET, "192.168.8.10", &ip) == 1);
    assert(dg_allocator_ack(allocator, mac, NULL, 0, ip, now) == 0);
    check_lease(ip, mac, fallback_id, sizeof(fallback_id), now + 60000);
    assert(changes == 4);
    assert(dg_allocator_release_mac(allocator, "02:00:00:00:00:01") == 0);
    check_empty();
    assert(changes == 5);
    assert(dg_allocator_ack(allocator, mac, NULL, 0, ip, now) == 0);
    check_lease(ip, mac, fallback_id, sizeof(fallback_id), now + 60000);
    assert(changes == 6);
    dg_allocator_reap_expired(allocator, now + 60001);
    check_empty();
    assert(changes == 7);
    check_empty();
    assert(changes == 7);
    reply = call("org.freedesktop.DBus.Properties", "Get", "DoesNotExist");
    assert(sd_bus_message_is_method_error(reply, SD_BUS_ERROR_UNKNOWN_PROPERTY));
    sd_bus_message_unref(reply);
    reply = call("org.freedesktop.DBus.Properties", "Set", "PoolSize");
    assert(sd_bus_message_is_method_error(reply, SD_BUS_ERROR_PROPERTY_READ_ONLY));
    sd_bus_message_unref(reply);
    sd_bus_slot_unref(match);
    sd_bus_flush_close_unref(client);
    dg_bus_cleanup(server);
    sd_bus_unref(connection);
    dg_allocator_cleanup(allocator);
    puts("D-Bus tests passed");
    return 0;
}
