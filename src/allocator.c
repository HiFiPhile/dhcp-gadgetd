#include "allocator.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#define DG_MAX_DYNAMIC_LEASES 512

typedef struct {
    int in_use;
    char mac[18];
    struct in_addr ip;
    uint64_t expires_at_ms;
} DgDynamicLease;

struct DgAllocator {
    uint32_t lease_seconds;
    uint64_t last_reap_ms;
    uint32_t pool_start_u32;
    uint32_t pool_end_u32;
    uint32_t next_candidate_u32;
    DgStaticLease static_leases[DG_MAX_STATIC_LEASES];
    size_t static_lease_count;
    DgDynamicLease dynamic_leases[DG_MAX_DYNAMIC_LEASES];
    size_t dynamic_count;
    DgActiveLease active_leases[DG_MAX_ACTIVE_LEASES];
    uint64_t lease_revision;
};

static uint32_t dg_ipv4_to_u32(struct in_addr addr) {
    return ntohl(addr.s_addr);
}

static struct in_addr dg_u32_to_ipv4(uint32_t host_order) {
    struct in_addr out;
    out.s_addr = htonl(host_order);
    return out;
}

static uint64_t dg_compute_expiry(uint64_t now_ms, uint32_t lease_seconds) {
    uint64_t delta = (uint64_t)lease_seconds * 1000ULL;
    if (UINT64_MAX - now_ms < delta) {
        return UINT64_MAX;
    }
    return now_ms + delta;
}

static int dg_find_dynamic_by_mac(const DgAllocator *allocator, const char *mac) {
    size_t i;
    for (i = 0; i < DG_MAX_DYNAMIC_LEASES; i++) {
        if (allocator->dynamic_leases[i].in_use && strcmp(allocator->dynamic_leases[i].mac, mac) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int dg_find_free_dynamic_slot(const DgAllocator *allocator) {
    size_t i;
    for (i = 0; i < DG_MAX_DYNAMIC_LEASES; i++) {
        if (!allocator->dynamic_leases[i].in_use) {
            return (int)i;
        }
    }
    return -1;
}

static int dg_dynamic_ip_in_use(const DgAllocator *allocator, uint32_t ip_u32) {
    size_t i;
    for (i = 0; i < DG_MAX_DYNAMIC_LEASES; i++) {
        if (allocator->dynamic_leases[i].in_use && dg_ipv4_to_u32(allocator->dynamic_leases[i].ip) == ip_u32) {
            return 1;
        }
    }
    return 0;
}

static int dg_static_ip_in_use(const DgAllocator *allocator, uint32_t ip_u32) {
    size_t i;
    for (i = 0; i < allocator->static_lease_count; i++) {
        if (dg_ipv4_to_u32(allocator->static_leases[i].ip) == ip_u32) {
            return 1;
        }
    }
    return 0;
}

int dg_allocator_init(DgAllocator **out_allocator, const DgConfig *cfg) {
    DgAllocator *allocator;

    if (out_allocator == NULL || cfg == NULL) {
        return -1;
    }

    allocator = calloc(1, sizeof(*allocator));
    if (allocator == NULL) {
        return -1;
    }

    allocator->lease_seconds = cfg->pool.lease_seconds;
    allocator->last_reap_ms = 0;
    allocator->pool_start_u32 = dg_ipv4_to_u32(cfg->pool.start);
    allocator->pool_end_u32 = dg_ipv4_to_u32(cfg->pool.end);
    allocator->next_candidate_u32 = allocator->pool_start_u32;
    allocator->static_lease_count = cfg->static_lease_count;
    memcpy(allocator->static_leases, cfg->static_leases, sizeof(cfg->static_leases));
    allocator->dynamic_count = 0;

    if (allocator->pool_end_u32 < allocator->pool_start_u32) {
        free(allocator);
        return -1;
    }

    *out_allocator = allocator;

    dg_log(DG_LOG_INFO, "allocator initialized (lease_seconds=%u static_leases=%zu)", allocator->lease_seconds,
           allocator->static_lease_count);
    return 0;
}

int dg_allocator_lookup_static(const DgAllocator *allocator, const char *usb_path, struct in_addr *out_ip) {
    size_t i;

    if (allocator == NULL || usb_path == NULL || out_ip == NULL) {
        return -1;
    }

    for (i = 0; i < allocator->static_lease_count; i++) {
        if (strcmp(allocator->static_leases[i].usb_path, usb_path) == 0) {
            *out_ip = allocator->static_leases[i].ip;
            return 0;
        }
    }

    return -1;
}

int dg_allocator_assign_dynamic(DgAllocator *allocator, const char *mac, uint64_t now_ms, struct in_addr *out_ip) {
    int existing_idx;
    int slot_idx;
    uint32_t range;
    uint32_t base;
    uint32_t offset;

    if (allocator == NULL || mac == NULL || out_ip == NULL) {
        return -1;
    }

    dg_allocator_reap_expired(allocator, now_ms);

    existing_idx = dg_find_dynamic_by_mac(allocator, mac);
    if (existing_idx >= 0) {
        allocator->dynamic_leases[existing_idx].expires_at_ms = dg_compute_expiry(now_ms, allocator->lease_seconds);
        *out_ip = allocator->dynamic_leases[existing_idx].ip;
        return 0;
    }

    slot_idx = dg_find_free_dynamic_slot(allocator);
    if (slot_idx < 0) {
        return -1;
    }

    range = allocator->pool_end_u32 - allocator->pool_start_u32 + 1U;
    if (range == 0U) {
        return -1;
    }

    base = allocator->next_candidate_u32;
    for (offset = 0U; offset < range; offset++) {
        uint32_t candidate = allocator->pool_start_u32 + ((base - allocator->pool_start_u32 + offset) % range);
        if (dg_static_ip_in_use(allocator, candidate)) {
            continue;
        }
        if (dg_dynamic_ip_in_use(allocator, candidate)) {
            continue;
        }

        allocator->dynamic_leases[slot_idx].in_use = 1;
        strncpy(allocator->dynamic_leases[slot_idx].mac, mac, sizeof(allocator->dynamic_leases[slot_idx].mac) - 1U);
        allocator->dynamic_leases[slot_idx].mac[sizeof(allocator->dynamic_leases[slot_idx].mac) - 1U] = '\0';
        allocator->dynamic_leases[slot_idx].ip = dg_u32_to_ipv4(candidate);
        allocator->dynamic_leases[slot_idx].expires_at_ms = dg_compute_expiry(now_ms, allocator->lease_seconds);
        allocator->dynamic_count++;

        allocator->next_candidate_u32 = candidate + 1U;
        if (allocator->next_candidate_u32 > allocator->pool_end_u32 ||
            allocator->next_candidate_u32 < allocator->pool_start_u32) {
            allocator->next_candidate_u32 = allocator->pool_start_u32;
        }

        *out_ip = allocator->dynamic_leases[slot_idx].ip;
        return 0;
    }

    return -1;
}

int dg_allocator_release_mac(DgAllocator *allocator, const char *mac) {
    int idx;
    int released = 0;
    size_t i;

    if (allocator == NULL || mac == NULL) {
        return -1;
    }

    for (i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
        DgActiveLease *lease = &allocator->active_leases[i];
        char text[18];
        if (!lease->expires_at_ms) continue;
        (void)snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                       lease->mac[0], lease->mac[1], lease->mac[2],
                       lease->mac[3], lease->mac[4], lease->mac[5]);
        if (lease->expires_at_ms != 0 && strcmp(text, mac) == 0) {
            lease->expires_at_ms = 0;
            allocator->lease_revision++;
            released = 1;
        }
    }
    idx = dg_find_dynamic_by_mac(allocator, mac);
    if (idx < 0) {
        return released ? 0 : -1;
    }

    allocator->dynamic_leases[idx].in_use = 0;
    allocator->dynamic_leases[idx].mac[0] = '\0';
    allocator->dynamic_leases[idx].ip.s_addr = 0;
    allocator->dynamic_leases[idx].expires_at_ms = 0;
    if (allocator->dynamic_count > 0) {
        allocator->dynamic_count--;
    }
    return 0;
}

size_t dg_allocator_active_dynamic_count(const DgAllocator *allocator) {
    if (allocator == NULL) {
        return 0;
    }
    return allocator->dynamic_count;
}

void dg_allocator_reap_expired(DgAllocator *allocator, uint64_t now_ms) {
    size_t i;

    if (allocator == NULL) {
        return;
    }

    for (i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
        DgActiveLease *lease = &allocator->active_leases[i];
        if (lease->expires_at_ms != 0 && lease->expires_at_ms <= now_ms) {
            lease->expires_at_ms = 0;
            allocator->lease_revision++;
        }
    }

    if (allocator->last_reap_ms == 0 || now_ms - allocator->last_reap_ms >= 5000U) {
        allocator->last_reap_ms = now_ms;

        for (i = 0; i < DG_MAX_DYNAMIC_LEASES; i++) {
            if (allocator->dynamic_leases[i].in_use && allocator->dynamic_leases[i].expires_at_ms <= now_ms) {
                allocator->dynamic_leases[i].in_use = 0;
                allocator->dynamic_leases[i].mac[0] = '\0';
                allocator->dynamic_leases[i].ip.s_addr = 0;
                allocator->dynamic_leases[i].expires_at_ms = 0;
                if (allocator->dynamic_count > 0) {
                    allocator->dynamic_count--;
                }
            }
        }

        dg_log(DG_LOG_DEBUG, "allocator reap tick");
    }
}

void dg_allocator_cleanup(DgAllocator *allocator) {
    free(allocator);
}

int dg_allocator_ack(DgAllocator *allocator, const uint8_t mac[6],
                     const uint8_t *client_id, size_t client_id_len,
                     struct in_addr ip, uint64_t now_ms) {
    DgActiveLease *slot = NULL;
    size_t i;

    if (!allocator || !mac || client_id_len > 255 || (client_id_len && !client_id)) {
        return -1;
    }
    dg_allocator_reap_expired(allocator, now_ms);
    for (i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
        DgActiveLease *lease = &allocator->active_leases[i];
        if (lease->expires_at_ms &&
            (memcmp(lease->mac, mac, 6) == 0 || lease->ip.s_addr == ip.s_addr)) {
            lease->expires_at_ms = 0;
            slot = lease;
        }
    }
    if (!slot) {
        for (i = 0; i < DG_MAX_ACTIVE_LEASES; i++) {
            if (!allocator->active_leases[i].expires_at_ms) {
                slot = &allocator->active_leases[i];
                break;
            }
        }
    }
    if (!slot) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, 6);
    if (client_id_len) {
        memcpy(slot->client_id, client_id, client_id_len);
        slot->client_id_len = client_id_len;
    } else {
        slot->client_id[0] = 1; /* Ethernet hardware type followed by MAC. */
        memcpy(slot->client_id + 1, mac, 6);
        slot->client_id_len = 7;
    }
    slot->ip = ip;
    slot->expires_at_ms = dg_compute_expiry(now_ms, allocator->lease_seconds);
    allocator->lease_revision++;
    return 0;
}

const DgActiveLease *dg_allocator_active_lease(const DgAllocator *allocator, size_t index) {
    if (!allocator || index >= DG_MAX_ACTIVE_LEASES || !allocator->active_leases[index].expires_at_ms) {
        return NULL;
    }
    return &allocator->active_leases[index];
}

uint64_t dg_allocator_lease_revision(const DgAllocator *allocator) {
    return allocator->lease_revision;
}
