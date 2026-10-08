#include "options.h"

#include <arpa/inet.h>
#include <string.h>

#define DG_DHCP_OPT_SUBNET_MASK 1U
#define DG_DHCP_OPT_ROUTER 3U
#define DG_DHCP_OPT_DNS 6U
#define DG_DHCP_OPT_LEASE_TIME 51U
#define DG_DHCP_OPT_MSG_TYPE 53U
#define DG_DHCP_OPT_SERVER_ID 54U
#define DG_DHCP_OPT_END 255U

size_t dg_options_encode(uint8_t *dst, size_t cap, uint8_t msg_type, const DgConfig *cfg, int include_lease) {
    size_t off = 0;

    if (cap < 1U) {
        return 0;
    }

    if (off + 3U > cap) {
        return 0;
    }
    dst[off++] = DG_DHCP_OPT_MSG_TYPE;
    dst[off++] = 1U;
    dst[off++] = msg_type;

    if (off + 6U > cap) {
        return 0;
    }
    dst[off++] = DG_DHCP_OPT_SERVER_ID;
    dst[off++] = 4U;
    memcpy(dst + off, &cfg->server_ip.s_addr, 4U);
    off += 4U;

    if (include_lease) {
        uint32_t lease_nbo = htonl(cfg->pool.lease_seconds);

        if (off + 6U > cap) {
            return 0;
        }
        dst[off++] = DG_DHCP_OPT_SUBNET_MASK;
        dst[off++] = 4U;
        memcpy(dst + off, &cfg->subnet_mask.s_addr, 4U);
        off += 4U;

        if (off + 6U > cap) {
            return 0;
        }
        dst[off++] = DG_DHCP_OPT_LEASE_TIME;
        dst[off++] = 4U;
        memcpy(dst + off, &lease_nbo, 4U);
        off += 4U;

        if (cfg->has_router) {
            if (off + 6U > cap) {
                return 0;
            }
            dst[off++] = DG_DHCP_OPT_ROUTER;
            dst[off++] = 4U;
            memcpy(dst + off, &cfg->router.s_addr, 4U);
            off += 4U;
        }

        if (cfg->dns_count > 0U) {
            size_t dns_len = cfg->dns_count * 4U;
            if (dns_len > 252U || off + 2U + dns_len > cap) {
                return 0;
            }
            dst[off++] = DG_DHCP_OPT_DNS;
            dst[off++] = (uint8_t)dns_len;
            memcpy(dst + off, cfg->dns, dns_len);
            off += dns_len;
        }

        {
            size_t i;
            for (i = 0; i < cfg->custom_option_count; i++) {
                const DgCustomOption *option = &cfg->custom_options[i];

                if (off + 2U + option->data_len > cap) {
                    return 0;
                }
                dst[off++] = option->code;
                dst[off++] = (uint8_t)option->data_len;
                memcpy(dst + off, option->data, option->data_len);
                off += option->data_len;
            }
        }
    }

    if (off + 1U > cap) {
        return 0;
    }
    dst[off++] = DG_DHCP_OPT_END;
    return off;
}
