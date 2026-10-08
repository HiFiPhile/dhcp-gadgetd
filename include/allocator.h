#ifndef DG_ALLOCATOR_H
#define DG_ALLOCATOR_H

#include <netinet/in.h>
#include <stdint.h>
#include <stddef.h>

#include "dg_types.h"

typedef struct DgAllocator DgAllocator;

#define DG_MAX_ACTIVE_LEASES (512 + DG_MAX_STATIC_LEASES)

typedef struct {
    uint8_t mac[6];
    uint8_t client_id[255];
    size_t client_id_len;
    struct in_addr ip;
    uint64_t expires_at_ms;
} DgActiveLease;

/* Only successful ACKs become active leases; offers remain reservations. */
int dg_allocator_ack(DgAllocator *allocator, const uint8_t mac[6],
                     const uint8_t *client_id, size_t client_id_len,
                     struct in_addr ip, uint64_t now_ms);
const DgActiveLease *dg_allocator_active_lease(const DgAllocator *allocator, size_t index);
uint64_t dg_allocator_lease_revision(const DgAllocator *allocator);

int dg_allocator_init(DgAllocator **out_allocator, const DgConfig *cfg);
int dg_allocator_lookup_static(const DgAllocator *allocator, const char *usb_path, struct in_addr *out_ip);
int dg_allocator_assign_dynamic(DgAllocator *allocator, const char *mac, uint64_t now_ms, struct in_addr *out_ip);
int dg_allocator_release_mac(DgAllocator *allocator, const char *mac);
size_t dg_allocator_active_dynamic_count(const DgAllocator *allocator);
void dg_allocator_reap_expired(DgAllocator *allocator, uint64_t now_ms);
void dg_allocator_cleanup(DgAllocator *allocator);

#endif