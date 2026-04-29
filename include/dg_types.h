#ifndef DG_TYPES_H
#define DG_TYPES_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define DG_MAX_DNS_SERVERS 4
#define DG_MAX_PATH_LEN 256
#define DG_MAX_STATIC_LEASES 128

typedef struct {
    char usb_path[DG_MAX_PATH_LEN];
    struct in_addr ip;
} DgStaticLease;

typedef struct {
    struct in_addr start;
    struct in_addr end;
    uint32_t lease_seconds;
} DgPool;

typedef struct {
    struct in_addr server_ip;
    struct in_addr subnet_network;
    struct in_addr subnet_mask;
    struct in_addr router;
    uint8_t subnet_prefix;
    int has_router;
    struct in_addr dns[DG_MAX_DNS_SERVERS];
    size_t dns_count;
    DgPool pool;
    DgStaticLease static_leases[DG_MAX_STATIC_LEASES];
    size_t static_lease_count;
    char config_path[DG_MAX_PATH_LEN];
} DgConfig;

#endif