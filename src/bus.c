#include "bus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "options.h"

struct DgBus {
    sd_bus *connection;
    sd_bus_slot *slot;
    const DgConfig *cfg;
    DgAllocator *allocator;
    uint64_t revision;
    int owns_connection;
    int owns_name;
};

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
}

static int append_leases(sd_bus_message *reply, DgBus *bus) {
    size_t i;
    int r;
    struct in_addr gateway = { .s_addr = 0 };

    if (bus->cfg->has_router) {
        gateway = bus->cfg->router;
    }
    r = sd_bus_message_open_container(reply, 'a', "(uayayayayt)");
    if (r < 0) return r;
    for (i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
        const DgActiveLease *lease = dg_allocator_active_lease(bus->allocator, i);
        uint64_t expiration;
        if (!lease) continue;
        expiration = lease->expires_at_ms > UINT64_MAX / 1000U ? UINT64_MAX : lease->expires_at_ms * 1000U;
        r = sd_bus_message_open_container(reply, 'r', "uayayayayt");
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "u", (uint32_t)AF_INET);
        if (r < 0) return r;
        r = sd_bus_message_append_array(reply, 'y', lease->client_id, lease->client_id_len);
        if (r < 0) return r;
        r = sd_bus_message_append_array(reply, 'y', &lease->ip, sizeof(lease->ip));
        if (r < 0) return r;
        r = sd_bus_message_append_array(reply, 'y', &gateway, sizeof(gateway));
        if (r < 0) return r;
        r = sd_bus_message_append_array(reply, 'y', lease->mac, sizeof(lease->mac));
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "t", expiration);
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
    }
    return sd_bus_message_close_container(reply);
}

static int append_options(sd_bus_message *reply, const DgConfig *cfg) {
    uint8_t options[336];
    size_t length = dg_options_encode(options, sizeof(options), 5, cfg, 1);
    size_t offset = 0;
    int r;

    if (!length) return -EINVAL;
    r = sd_bus_message_open_container(reply, 'a', "(yay)");
    if (r < 0) return r;
    while (offset < length && options[offset] != 255) {
        uint8_t code = options[offset++];
        uint8_t size = options[offset++];
        /* Message type is per packet, not a configured server option. */
        if (code != 53) {
            r = sd_bus_message_open_container(reply, 'r', "yay");
            if (r < 0) return r;
            r = sd_bus_message_append(reply, "y", code);
            if (r < 0) return r;
            r = sd_bus_message_append_array(reply, 'y', options + offset, size);
            if (r < 0) return r;
            r = sd_bus_message_close_container(reply);
            if (r < 0) return r;
        }
        offset += size;
    }
    return sd_bus_message_close_container(reply);
}

static int append_config_array(sd_bus_message *reply, const DgConfig *cfg, const char *property) {
    int r;

    if (strcmp(property, "DNS") == 0) {
        r = sd_bus_message_open_container(reply, 'a', "ay");
        if (r < 0) return r;
        for (size_t i = 0; i < cfg->dns_count; i++) {
            r = sd_bus_message_append_array(reply, 'y', &cfg->dns[i], 4);
            if (r < 0) return r;
        }
    } else if (strcmp(property, "StaticLeases") == 0) {
        r = sd_bus_message_open_container(reply, 'a', "(say)");
        if (r < 0) return r;
        for (size_t i = 0; i < cfg->static_lease_count; i++) {
            r = sd_bus_message_open_container(reply, 'r', "say");
            if (r < 0) return r;
            r = sd_bus_message_append(reply, "s", cfg->static_leases[i].usb_path);
            if (r < 0) return r;
            r = sd_bus_message_append_array(reply, 'y', &cfg->static_leases[i].ip, 4);
            if (r < 0) return r;
            r = sd_bus_message_close_container(reply);
            if (r < 0) return r;
        }
    } else if (strcmp(property, "Drivers") == 0) {
        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0) return r;
        for (size_t i = 0; i < cfg->driver_count; i++) {
            r = sd_bus_message_append(reply, "s", cfg->drivers[i]);
            if (r < 0) return r;
        }
    } else {
        r = sd_bus_message_open_container(reply, 'a', "(yay)");
        if (r < 0) return r;
        for (size_t i = 0; i < cfg->custom_option_count; i++) {
            const DgCustomOption *option = &cfg->custom_options[i];
            r = sd_bus_message_open_container(reply, 'r', "yay");
            if (r < 0) return r;
            r = sd_bus_message_append(reply, "y", option->code);
            if (r < 0) return r;
            r = sd_bus_message_append_array(reply, 'y', option->data, option->data_len);
            if (r < 0) return r;
            r = sd_bus_message_close_container(reply);
            if (r < 0) return r;
        }
    }
    return sd_bus_message_close_container(reply);
}

static int get_property(sd_bus *connection, const char *path, const char *interface,
                        const char *property, sd_bus_message *reply, void *userdata,
                        sd_bus_error *error) {
    DgBus *bus = userdata;
    (void)connection;
    (void)path;
    (void)interface;
    (void)error;

    const DgConfig *cfg = bus->cfg;
    if (strcmp(property, "ServerAddress") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->server_ip, 4);
    if (strcmp(property, "SubnetAddress") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->subnet_network, 4);
    if (strcmp(property, "SubnetMask") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->subnet_mask, 4);
    if (strcmp(property, "SubnetPrefixLength") == 0)
        return sd_bus_message_append(reply, "y", cfg->subnet_prefix);
    if (strcmp(property, "Router") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->router, cfg->has_router ? 4 : 0);
    if (strcmp(property, "PoolStart") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->pool.start, 4);
    if (strcmp(property, "PoolEnd") == 0)
        return sd_bus_message_append_array(reply, 'y', &cfg->pool.end, 4);
    if (strcmp(property, "LeaseTime") == 0)
        return sd_bus_message_append(reply, "u", cfg->pool.lease_seconds);
    if (strcmp(property, "Bridge") == 0)
        return sd_bus_message_append(reply, "s", cfg->bridge_name);
    if (strcmp(property, "ConfigFile") == 0)
        return sd_bus_message_append(reply, "s", cfg->config_path);
    if (strcmp(property, "DNS") == 0 || strcmp(property, "StaticLeases") == 0 ||
        strcmp(property, "CustomOptions") == 0 || strcmp(property, "Drivers") == 0)
        return append_config_array(reply, cfg, property);

    if (strcmp(property, "Leases") == 0) return append_leases(reply, bus);
    if (strcmp(property, "Options") == 0) return append_options(reply, bus->cfg);
    if (strcmp(property, "PoolSize") == 0) {
        return sd_bus_message_append(reply, "u", ntohl(bus->cfg->pool.end.s_addr) -
                                     ntohl(bus->cfg->pool.start.s_addr) + 1U);
    }
    if (strcmp(property, "PoolOffset") == 0) {
        return sd_bus_message_append(reply, "u", ntohl(bus->cfg->pool.start.s_addr) -
                                     ntohl(bus->cfg->subnet_network.s_addr));
    }
    return -EINVAL;
}

static const sd_bus_vtable server_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Leases", "a(uayayayayt)", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Options", "a(yay)", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PoolSize", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PoolOffset", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServerAddress", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SubnetAddress", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SubnetMask", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SubnetPrefixLength", "y", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Router", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("DNS", "aay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PoolStart", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PoolEnd", "ay", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("LeaseTime", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("StaticLeases", "a(say)", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CustomOptions", "a(yay)", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Bridge", "s", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Drivers", "as", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ConfigFile", "s", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

int dg_bus_init(DgBus **out, const DgConfig *cfg, DgAllocator *allocator, sd_bus *connection) {
    DgBus *bus = calloc(1, sizeof(*bus));
    int r;

    *out = NULL;
    if (!bus) return -ENOMEM;
    bus->cfg = cfg;
    bus->allocator = allocator;
    bus->revision = dg_allocator_lease_revision(allocator);
    if (connection) {
        bus->connection = sd_bus_ref(connection);
    } else {
        bus->owns_connection = 1;
        r = sd_bus_open_system(&bus->connection);
        if (r < 0) goto fail;
    }
    r = sd_bus_add_object_vtable(bus->connection, &bus->slot, DG_BUS_PATH,
                                 DG_BUS_INTERFACE, server_vtable, bus);
    if (r < 0) goto fail;
    r = sd_bus_request_name(bus->connection, DG_BUS_NAME, 0);
    if (r < 0) goto fail;
    bus->owns_name = 1;
    *out = bus;
    return 0;
fail:
    dg_log(DG_LOG_ERROR, "D-Bus initialization failed: %s", strerror(-r));
    dg_bus_cleanup(bus);
    return r;
}

int dg_bus_process(DgBus *bus) {
    int r;
    uint64_t revision;

    dg_allocator_reap_expired(bus->allocator, monotonic_usec() / 1000U);
    revision = dg_allocator_lease_revision(bus->allocator);
    if (bus->revision != revision) {
        r = sd_bus_emit_properties_changed(bus->connection, DG_BUS_PATH, DG_BUS_INTERFACE, "Leases", NULL);
        if (r < 0) return r;
        bus->revision = revision;
    }
    /* Bound work per tick so a busy bus cannot starve DHCP sockets. */
    for (unsigned int i = 0; i < 64; i++) {
        r = sd_bus_process(bus->connection, NULL);
        if (r <= 0) return r;
    }
    return 0;
}

int dg_bus_prepare_poll(DgBus *bus, struct pollfd *pfd, int *timeout_ms) {
    uint64_t deadline;
    int events;
    int r;

    pfd->fd = sd_bus_get_fd(bus->connection);
    if (pfd->fd < 0) return pfd->fd;
    events = sd_bus_get_events(bus->connection);
    if (events < 0) return events;
    pfd->events = (short)events;
    pfd->revents = 0;
    r = sd_bus_get_timeout(bus->connection, &deadline);
    if (r < 0) return r;
    if (deadline != UINT64_MAX) {
        uint64_t now = monotonic_usec();
        uint64_t wait_ms = deadline > now ? (deadline - now) / 1000U : 0;
        if (wait_ms < (uint64_t)*timeout_ms) *timeout_ms = (int)wait_ms;
    }
    return 0;
}

void dg_bus_cleanup(DgBus *bus) {
    if (!bus) return;
    sd_bus_slot_unref(bus->slot);
    if (bus->owns_name) (void)sd_bus_release_name(bus->connection, DG_BUS_NAME);
    if (bus->owns_connection) {
        sd_bus_flush_close_unref(bus->connection);
    } else {
        sd_bus_unref(bus->connection);
    }
    free(bus);
}
