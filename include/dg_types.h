#ifndef DG_TYPES_H
#define DG_TYPES_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define DG_MAX_DNS_SERVERS 4
#define DG_MAX_CUSTOM_OPTIONS 32
#define DG_MAX_DHCP_OPTION_DATA_LEN 255
#define DG_MAX_CUSTOM_OPTIONS_ENCODED_LEN 262
#define DG_MAX_PATH_LEN 256
#define DG_MAX_STATIC_LEASES 128
#define DG_MAX_IFNAME_LEN 16 /* IFNAMSIZ, including the terminating NUL */
#define DG_MAX_DRIVERS 8
#define DG_MAX_DRIVER_NAME_LEN 32

typedef struct {
    uint8_t code;
    uint8_t data[DG_MAX_DHCP_OPTION_DATA_LEN];
    size_t data_len;
} DgCustomOption;

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
    DgCustomOption custom_options[DG_MAX_CUSTOM_OPTIONS];
    size_t custom_option_count;
    DgPool pool;
    DgStaticLease static_leases[DG_MAX_STATIC_LEASES];
    size_t static_lease_count;
    char bridge_name[DG_MAX_IFNAME_LEN];
    char drivers[DG_MAX_DRIVERS][DG_MAX_DRIVER_NAME_LEN];
    size_t driver_count;
    char config_path[DG_MAX_PATH_LEN];
} DgConfig;

#endif
