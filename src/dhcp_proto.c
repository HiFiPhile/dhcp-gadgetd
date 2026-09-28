#include "dhcp_proto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "interface.h"
#include "log.h"

#define DG_DHCP_IPV4_MIN_HEADER_LEN 20U
#define DG_DHCP_UDP_HEADER_LEN 8U
#define DG_DHCP_BOOTP_FIXED_LEN 236U
#define DG_DHCP_COOKIE_LEN 4U
#define DG_DHCP_BASE_LEN (DG_DHCP_BOOTP_FIXED_LEN + DG_DHCP_COOKIE_LEN)
#define DG_DHCP_MAX_FRAME_LEN 1600U

#define DG_DHCP_SERVER_PORT 67U
#define DG_DHCP_CLIENT_PORT 68U

#define DG_DHCP_OPT_PAD 0U
#define DG_DHCP_OPT_SUBNET_MASK 1U
#define DG_DHCP_OPT_ROUTER 3U
#define DG_DHCP_OPT_DNS 6U
#define DG_DHCP_OPT_REQ_IP 50U
#define DG_DHCP_OPT_LEASE_TIME 51U
#define DG_DHCP_OPT_MSG_TYPE 53U
#define DG_DHCP_OPT_SERVER_ID 54U
#define DG_DHCP_OPT_END 255U

#define DG_DHCP_TYPE_DISCOVER 1U
#define DG_DHCP_TYPE_OFFER 2U
#define DG_DHCP_TYPE_REQUEST 3U
#define DG_DHCP_TYPE_ACK 5U
#define DG_DHCP_TYPE_NAK 6U
#define DG_DHCP_TYPE_RELEASE 7U

#define DG_DHCP_MAGIC_COOKIE 0x63825363U

typedef struct {
    uint32_t xid;
    uint16_t flags;
    struct in_addr ciaddr;
    struct in_addr requested_ip;
    struct in_addr server_id;
    uint8_t msg_type;
    int has_requested_ip;
    int has_server_id;
    uint8_t chaddr[16];
    int ifindex;
    char ifname[IF_NAMESIZE];
    char client_mac[18];
} DgDhcpRequest;

typedef struct {
    int dry_run;
    DgConfig cfg;
    DgAllocator *allocator;
} DgDhcpProtoState;

static DgDhcpProtoState g_state = {0};

static uint16_t dg_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

static uint32_t dg_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}

static void dg_write_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)((value >> 8U) & 0xFFU);
    p[1] = (uint8_t)(value & 0xFFU);
}

static void dg_write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)((value >> 24U) & 0xFFU);
    p[1] = (uint8_t)((value >> 16U) & 0xFFU);
    p[2] = (uint8_t)((value >> 8U) & 0xFFU);
    p[3] = (uint8_t)(value & 0xFFU);
}

static uint16_t dg_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1U < len; i += 2U) {
        sum += (uint32_t)dg_read_be16(data + i);
    }
    if ((len & 1U) != 0U) {
        sum += (uint32_t)data[len - 1U] << 8U;
    }

    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }

    return (uint16_t)(~sum & 0xFFFFU);
}

static uint16_t dg_udp_checksum(const struct in_addr src_ip, const struct in_addr dst_ip, const uint8_t *udp_packet,
                                size_t udp_len) {
    uint32_t sum = 0;
    size_t i;
    const uint8_t *src = (const uint8_t *)&src_ip.s_addr;
    const uint8_t *dst = (const uint8_t *)&dst_ip.s_addr;

    sum += (uint32_t)dg_read_be16(src + 0);
    sum += (uint32_t)dg_read_be16(src + 2);
    sum += (uint32_t)dg_read_be16(dst + 0);
    sum += (uint32_t)dg_read_be16(dst + 2);
    sum += 17U;
    sum += (uint32_t)udp_len;

    for (i = 0; i + 1U < udp_len; i += 2U) {
        sum += (uint32_t)dg_read_be16(udp_packet + i);
    }
    if ((udp_len & 1U) != 0U) {
        sum += (uint32_t)udp_packet[udp_len - 1U] << 8U;
    }

    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }

    {
        uint16_t out = (uint16_t)(~sum & 0xFFFFU);
        if (out == 0U) {
            out = 0xFFFFU;
        }
        return out;
    }
}

static int dg_mac_to_string(const uint8_t *mac, char *out, size_t out_len) {
    int n;
    if (out_len < 18U) {
        return -1;
    }
    n = snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n < 0 || n >= (int)out_len) {
        return -1;
    }
    return 0;
}

static int dg_parse_dhcp_request(const uint8_t *frame, size_t frame_len, const struct sockaddr_ll *addr,
                                 DgDhcpRequest *out_req) {
    size_t ip_ihl;
    uint16_t ip_total_len;
    size_t udp_off;
    uint16_t udp_len;
    const uint8_t *payload;
    size_t payload_len;
    size_t opt_off;

    memset(out_req, 0, sizeof(*out_req));

    if (frame_len < DG_DHCP_IPV4_MIN_HEADER_LEN) {
        return 0;
    }

    /* Socket BPF already restricts traffic to DHCPv4 requests; keep length checks here. */
    ip_ihl = (size_t)(frame[0U] & 0x0FU) * 4U;
    if (ip_ihl < DG_DHCP_IPV4_MIN_HEADER_LEN || frame_len < ip_ihl) {
        return 0;
    }

    ip_total_len = dg_read_be16(frame + 2U);
    if ((size_t)ip_total_len < ip_ihl + DG_DHCP_UDP_HEADER_LEN || frame_len < (size_t)ip_total_len) {
        return 0;
    }

    udp_off = ip_ihl;
    udp_len = dg_read_be16(frame + udp_off + 4U);
    if (udp_len < DG_DHCP_UDP_HEADER_LEN + DG_DHCP_BASE_LEN || (size_t)udp_len > (size_t)ip_total_len - ip_ihl) {
        return 0;
    }

    payload = frame + udp_off + DG_DHCP_UDP_HEADER_LEN;
    payload_len = (size_t)udp_len - DG_DHCP_UDP_HEADER_LEN;

    if (payload_len < DG_DHCP_BASE_LEN) {
        return 0;
    }

    out_req->xid = dg_read_be32(payload + 4U);
    out_req->flags = dg_read_be16(payload + 10U);
    memcpy(&out_req->ciaddr.s_addr, payload + 12U, 4U);
    memcpy(out_req->chaddr, payload + 28U, 16U);
    out_req->ifindex = addr->sll_ifindex;
    if (if_indextoname((unsigned int)out_req->ifindex, out_req->ifname) == NULL) {
        out_req->ifname[0] = '\0';
    }
    if (dg_mac_to_string(out_req->chaddr, out_req->client_mac, sizeof(out_req->client_mac)) != 0) {
        return 0;
    }

    opt_off = DG_DHCP_BASE_LEN;
    while (opt_off < payload_len) {
        uint8_t opt = payload[opt_off++];
        if (opt == DG_DHCP_OPT_PAD) {
            continue;
        }
        if (opt == DG_DHCP_OPT_END) {
            break;
        }
        if (opt_off >= payload_len) {
            break;
        }

        {
            uint8_t opt_len = payload[opt_off++];
            if (opt_off + (size_t)opt_len > payload_len) {
                break;
            }

            switch (opt) {
                case DG_DHCP_OPT_MSG_TYPE:
                    if (opt_len >= 1U) {
                        out_req->msg_type = payload[opt_off];
                    }
                    break;
                case DG_DHCP_OPT_REQ_IP:
                    if (opt_len == 4U) {
                        memcpy(&out_req->requested_ip.s_addr, payload + opt_off, 4U);
                        out_req->has_requested_ip = 1;
                    }
                    break;
                case DG_DHCP_OPT_SERVER_ID:
                    if (opt_len == 4U) {
                        memcpy(&out_req->server_id.s_addr, payload + opt_off, 4U);
                        out_req->has_server_id = 1;
                    }
                    break;
                default:
                    break;
            }

            opt_off += (size_t)opt_len;
        }
    }

    if (out_req->msg_type == 0U) {
        return 0;
    }

    return 1;
}

static size_t dg_encode_options(uint8_t *dst, size_t cap, uint8_t msg_type, const DgConfig *cfg, int include_lease) {
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

static int dg_send_reply(const DgDhcpRequest *req, uint8_t reply_type, struct in_addr yiaddr, int include_lease) {
    uint8_t frame[600];
    uint8_t broadcast_hwaddr[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    struct sockaddr_ll out_addr;
    const DgInterfaceEntry *iface;
    size_t options_len;
    size_t dhcp_len;
    size_t udp_len;
    size_t ip_len;
    size_t frame_len;
    struct in_addr dst_ip;
    int send_fd;

    memset(frame, 0, sizeof(frame));
    memset(&out_addr, 0, sizeof(out_addr));

    frame[0U] = 0x45U;
    frame[1U] = 0U;
    dg_write_be16(frame + 4U, 0U);
    dg_write_be16(frame + 6U, 0U);
    frame[8U] = 64U;
    frame[9U] = 17U;
    dg_write_be16(frame + 10U, 0U);
    memcpy(frame + 12U, &g_state.cfg.server_ip.s_addr, 4U);
    dst_ip.s_addr = htonl(0xFFFFFFFFU);
    memcpy(frame + 16U, &dst_ip.s_addr, 4U);

    dg_write_be16(frame + 20U, DG_DHCP_SERVER_PORT);
    dg_write_be16(frame + 22U, DG_DHCP_CLIENT_PORT);
    dg_write_be16(frame + 24U, 0U);
    dg_write_be16(frame + 26U, 0U);

    frame[28U + 0U] = 2U;
    frame[28U + 1U] = 1U;
    frame[28U + 2U] = 6U;
    frame[28U + 3U] = 0U;
    dg_write_be32(frame + 28U + 4U, req->xid);
    dg_write_be16(frame + 28U + 8U, 0U);
    dg_write_be16(frame + 28U + 10U, req->flags);
    dg_write_be32(frame + 28U + 12U, 0U);
    memcpy(frame + 28U + 16U, &yiaddr.s_addr, 4U);
    memcpy(frame + 28U + 20U, &g_state.cfg.server_ip.s_addr, 4U);
    dg_write_be32(frame + 28U + 24U, 0U);
    memcpy(frame + 28U + 28U, req->chaddr, 16U);
    dg_write_be32(frame + 28U + DG_DHCP_BOOTP_FIXED_LEN, DG_DHCP_MAGIC_COOKIE);

    options_len = dg_encode_options(frame + 28U + DG_DHCP_BASE_LEN,
                                    sizeof(frame) - (28U + DG_DHCP_BASE_LEN), reply_type, &g_state.cfg,
                                    include_lease);
    if (options_len == 0U) {
        return -1;
    }

    dhcp_len = DG_DHCP_BASE_LEN + options_len;
    udp_len = DG_DHCP_UDP_HEADER_LEN + dhcp_len;
    ip_len = DG_DHCP_IPV4_MIN_HEADER_LEN + udp_len;
    frame_len = ip_len;

    dg_write_be16(frame + 2U, (uint16_t)ip_len);
    dg_write_be16(frame + 24U, (uint16_t)udp_len);

    dg_write_be16(frame + 10U, 0U);
    dg_write_be16(frame + 10U, dg_checksum(frame, DG_DHCP_IPV4_MIN_HEADER_LEN));

    dg_write_be16(frame + 26U, 0U);
    dg_write_be16(frame + 26U, dg_udp_checksum(g_state.cfg.server_ip, dst_ip, frame + 20U, udp_len));

    out_addr.sll_family = AF_PACKET;
    out_addr.sll_protocol = htons(ETH_P_IP);
    out_addr.sll_ifindex = req->ifindex;
    out_addr.sll_halen = 6U;
    memcpy(out_addr.sll_addr, broadcast_hwaddr, 6U);

    iface = dg_interface_by_ifindex(req->ifindex);
    send_fd = (iface != NULL) ? iface->sock_fd : -1;
    if (send_fd < 0) {
        dg_log(DG_LOG_WARN, "no sender socket for ifindex=%d", req->ifindex);
        return -1;
    }

    if (sendto(send_fd, frame, frame_len, 0, (const struct sockaddr *)&out_addr, sizeof(out_addr)) < 0) {
        dg_log(DG_LOG_WARN, "sendto failed for ifindex=%d", req->ifindex);
        return -1;
    }

    return 0;
}

static int dg_is_server_id_match(const DgDhcpRequest *req) {
    if (!req->has_server_id) {
        return 1;
    }
    return req->server_id.s_addr == g_state.cfg.server_ip.s_addr;
}

static void dg_log_ip_action(const char *verb, uint8_t msg_type, const DgDhcpRequest *req, struct in_addr ip) {
    char ip_buf[INET_ADDRSTRLEN];
    const char *mtype = "UNKNOWN";

    if (msg_type == DG_DHCP_TYPE_OFFER) {
        mtype = "OFFER";
    } else if (msg_type == DG_DHCP_TYPE_ACK) {
        mtype = "ACK";
    } else if (msg_type == DG_DHCP_TYPE_NAK) {
        mtype = "NAK";
    }

    if (inet_ntop(AF_INET, &ip, ip_buf, sizeof(ip_buf)) == NULL) {
        strcpy(ip_buf, "<invalid>");
    }

    dg_log(DG_LOG_INFO, "%s %s mac=%s if=%s ip=%s", verb, mtype, req->client_mac,
           req->ifname[0] != '\0' ? req->ifname : "<unknown>", ip_buf);
}

static void dg_ipv4_to_text(struct in_addr ip, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (inet_ntop(AF_INET, &ip, out, (socklen_t)out_len) == NULL) {
        strncpy(out, "<invalid>", out_len - 1U);
        out[out_len - 1U] = '\0';
    }
}

static const char *dg_request_lease_key(const DgDhcpRequest *req, int *out_is_usb_path) {
    const DgInterfaceEntry *iface = dg_interface_by_ifindex(req->ifindex);
    if (iface != NULL && iface->usb_path[0] != '\0') {
        if (out_is_usb_path != NULL) {
            *out_is_usb_path = 1;
        }
        dg_log(DG_LOG_DEBUG, "lease key resolved mac=%s if=%s usb_path=%s", req->client_mac,
               req->ifname[0] != '\0' ? req->ifname : "<unknown>", iface->usb_path);
        return iface->usb_path;
    }

    if (out_is_usb_path != NULL) {
        *out_is_usb_path = 0;
    }
    dg_log(DG_LOG_DEBUG, "lease key fallback mac=%s if=%s key=%s", req->client_mac,
           req->ifname[0] != '\0' ? req->ifname : "<unknown>", req->ifname[0] != '\0' ? req->ifname : "<empty>");
    return req->ifname;
}

static int dg_handle_discover(const DgDhcpRequest *req, uint64_t now_ms) {
    struct in_addr lease_ip;
    char lease_ip_buf[INET_ADDRSTRLEN];
    int lease_key_is_usb_path = 0;
    int used_static_lease = 0;
    const char *lease_key = dg_request_lease_key(req, &lease_key_is_usb_path);

    if (dg_allocator_lookup_static(g_state.allocator, lease_key, &lease_ip) == 0) {
        used_static_lease = 1;
    } else {
        if (dg_allocator_assign_dynamic(g_state.allocator, req->client_mac, now_ms, &lease_ip) != 0) {
            dg_log(DG_LOG_WARN, "no available lease for DISCOVER mac=%s", req->client_mac);
            return 0;
        }
    }

    dg_ipv4_to_text(lease_ip, lease_ip_buf, sizeof(lease_ip_buf));
    dg_log(DG_LOG_DEBUG, "%s lease selected for DISCOVER mac=%s if=%s key_type=%s key=%s ip=%s",
           used_static_lease ? "static" : "dynamic", req->client_mac,
           req->ifname[0] != '\0' ? req->ifname : "<unknown>", lease_key_is_usb_path ? "usb_path" : "ifname",
           lease_key, lease_ip_buf);

    if (dg_send_reply(req, DG_DHCP_TYPE_OFFER, lease_ip, 1) == 0) {
        dg_log_ip_action("sent", DG_DHCP_TYPE_OFFER, req, lease_ip);
    }
    return 0;
}

static int dg_handle_request(const DgDhcpRequest *req, uint64_t now_ms) {
    struct in_addr assigned_ip;
    struct in_addr wanted_ip;
    char assigned_ip_buf[INET_ADDRSTRLEN];
    char wanted_ip_buf[INET_ADDRSTRLEN];
    int have_wanted = 0;
    int lease_key_is_usb_path = 0;
    int used_static_lease = 0;
    const char *lease_key = dg_request_lease_key(req, &lease_key_is_usb_path);

    if (!dg_is_server_id_match(req)) {
        return 0;
    }

    if (req->has_requested_ip) {
        wanted_ip = req->requested_ip;
        have_wanted = 1;
    } else if (req->ciaddr.s_addr != 0U) {
        wanted_ip = req->ciaddr;
        have_wanted = 1;
    }

    if (dg_allocator_lookup_static(g_state.allocator, lease_key, &assigned_ip) == 0) {
        used_static_lease = 1;
    } else {
        if (dg_allocator_assign_dynamic(g_state.allocator, req->client_mac, now_ms, &assigned_ip) != 0) {
            struct in_addr zero_ip;
            zero_ip.s_addr = 0U;
            (void)dg_send_reply(req, DG_DHCP_TYPE_NAK, zero_ip, 0);
            dg_log_ip_action("sent", DG_DHCP_TYPE_NAK, req, zero_ip);
            return 0;
        }
    }

    dg_ipv4_to_text(assigned_ip, assigned_ip_buf, sizeof(assigned_ip_buf));
    dg_log(DG_LOG_DEBUG, "%s lease selected for REQUEST mac=%s if=%s key_type=%s key=%s ip=%s",
           used_static_lease ? "static" : "dynamic", req->client_mac,
           req->ifname[0] != '\0' ? req->ifname : "<unknown>", lease_key_is_usb_path ? "usb_path" : "ifname",
           lease_key, assigned_ip_buf);

    if (have_wanted && wanted_ip.s_addr != assigned_ip.s_addr) {
        struct in_addr zero_ip;
        zero_ip.s_addr = 0U;
        dg_ipv4_to_text(wanted_ip, wanted_ip_buf, sizeof(wanted_ip_buf));
        dg_log(DG_LOG_DEBUG, "request IP mismatch mac=%s if=%s requested=%s assigned=%s", req->client_mac,
               req->ifname[0] != '\0' ? req->ifname : "<unknown>", wanted_ip_buf, assigned_ip_buf);
        (void)dg_send_reply(req, DG_DHCP_TYPE_NAK, zero_ip, 0);
        dg_log_ip_action("sent", DG_DHCP_TYPE_NAK, req, zero_ip);
        return 0;
    }

    if (dg_send_reply(req, DG_DHCP_TYPE_ACK, assigned_ip, 1) == 0) {
        dg_log_ip_action("sent", DG_DHCP_TYPE_ACK, req, assigned_ip);
    }
    return 0;
}

static int dg_handle_release(const DgDhcpRequest *req) {
    if (dg_allocator_release_mac(g_state.allocator, req->client_mac) == 0) {
        dg_log(DG_LOG_INFO, "released lease mac=%s", req->client_mac);
    }
    return 0;
}

static int dg_handle_frame(const uint8_t *frame, size_t frame_len, const struct sockaddr_ll *addr, uint64_t now_ms) {
    DgDhcpRequest req;
    int parse_rc;

    parse_rc = dg_parse_dhcp_request(frame, frame_len, addr, &req);
    if (parse_rc <= 0) {
        return 0;
    }

    if (dg_interface_by_ifindex(req.ifindex) == NULL) {
        return 0;
    }

    switch (req.msg_type) {
        case DG_DHCP_TYPE_DISCOVER:
            return dg_handle_discover(&req, now_ms);
        case DG_DHCP_TYPE_REQUEST:
            return dg_handle_request(&req, now_ms);
        case DG_DHCP_TYPE_RELEASE:
            return dg_handle_release(&req);
        default:
            return 0;
    }
}

int dg_dhcp_proto_init(const DgConfig *cfg, DgAllocator *allocator) {
    int probe_fd;

    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *cfg;
    g_state.allocator = allocator;

    probe_fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (probe_fd < 0) {
        if (errno == EPERM || errno == EACCES) {
            g_state.dry_run = 1;
            dg_log(DG_LOG_WARN, "dhcp protocol running in dry-run mode (no CAP_NET_RAW)");
            return 0;
        }
        dg_log(DG_LOG_ERROR, "failed to open AF_PACKET socket");
        return -1;
    }
    close(probe_fd);

    dg_log(DG_LOG_INFO, "dhcp protocol module initialized");
    return 0;
}

void dg_dhcp_proto_shutdown(void) {
    g_state.allocator = NULL;
    g_state.dry_run = 0;
    dg_log(DG_LOG_INFO, "dhcp protocol module shutdown");
}

int dg_dhcp_proto_drain_socket(const DgInterfaceEntry *entry, uint64_t now_ms) {
    int frames = 0;

    if (g_state.dry_run) {
        return 0;
    }

    while (frames < 16) {
        uint8_t frame[DG_DHCP_MAX_FRAME_LEN];
        struct sockaddr_ll addr;
        socklen_t addr_len = (socklen_t)sizeof(addr);
        ssize_t nread;

        memset(&addr, 0, sizeof(addr));
        nread = recvfrom(entry->sock_fd, frame, sizeof(frame), 0, (struct sockaddr *)&addr, &addr_len);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            return -1;
        }

        if (addr.sll_pkttype == PACKET_OUTGOING) {
            frames++;
            continue;
        }

        (void)dg_handle_frame(frame, (size_t)nread, &addr, now_ms);
        frames++;
    }

    return 0;
}
