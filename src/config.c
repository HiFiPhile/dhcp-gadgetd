#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static uint32_t dg_ipv4_to_u32(struct in_addr addr) {
    return ntohl(addr.s_addr);
}

static struct in_addr dg_u32_to_ipv4(uint32_t host_order) {
    struct in_addr out;

    out.s_addr = htonl(host_order);
    return out;
}

static int dg_parse_ipv4_string(const char *text, struct in_addr *out_addr) {
    if (text == NULL || out_addr == NULL) {
        return -1;
    }
    if (inet_pton(AF_INET, text, out_addr) != 1) {
        return -1;
    }
    return 0;
}

static int dg_parse_u32_string(const char *text, uint32_t *out_value) {
    char *endptr;
    unsigned long value;

    if (text == NULL || out_value == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || value > UINT_MAX) {
        return -1;
    }

    *out_value = (uint32_t)value;
    return 0;
}

static int dg_hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int dg_parse_subnet_cidr_string(const char *text, struct in_addr *out_network, struct in_addr *out_mask,
                                       uint8_t *out_prefix) {
    char tmp[48];
    char *slash;
    char *endptr;
    unsigned long prefix_num;
    struct in_addr ip;
    uint32_t mask_u32;
    uint32_t network_u32;

    if (text == NULL || out_network == NULL || out_mask == NULL || out_prefix == NULL) {
        return -1;
    }

    if (strlen(text) >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, text, strlen(text) + 1U);

    slash = strchr(tmp, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    slash++;

    if (dg_parse_ipv4_string(tmp, &ip) != 0) {
        return -1;
    }

    errno = 0;
    prefix_num = strtoul(slash, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || prefix_num > 32UL) {
        return -1;
    }

    if (prefix_num == 0UL) {
        mask_u32 = 0U;
    } else {
        mask_u32 = 0xFFFFFFFFU << (32U - (uint32_t)prefix_num);
    }

    network_u32 = dg_ipv4_to_u32(ip) & mask_u32;
    *out_network = dg_u32_to_ipv4(network_u32);
    *out_mask = dg_u32_to_ipv4(mask_u32);
    *out_prefix = (uint8_t)prefix_num;
    return 0;
}

/* Same rules as the kernel's dev_valid_name(). */
static int dg_parse_ifname(const char *text, char *out, size_t out_size) {
    size_t len = strlen(text);
    size_t i;

    if (len == 0U || len >= out_size || strcmp(text, ".") == 0 || strcmp(text, "..") == 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (text[i] == '/' || text[i] == ':' || isspace((unsigned char)text[i])) {
            return -1;
        }
    }

    memcpy(out, text, len + 1U);
    return 0;
}

static int dg_ipv4_in_subnet(struct in_addr ip, struct in_addr network, struct in_addr mask) {
    return (dg_ipv4_to_u32(ip) & dg_ipv4_to_u32(mask)) == dg_ipv4_to_u32(network);
}

static int dg_read_file_bytes(const char *path, char **out_buf, size_t *out_len) {
    FILE *fh;
    char *buf;
    size_t cap = 4096;
    size_t len = 0;

    if (path == NULL || out_buf == NULL || out_len == NULL) {
        return -1;
    }

    fh = fopen(path, "rb");
    if (fh == NULL) {
        return -1;
    }

    buf = malloc(cap + 1U);
    if (buf == NULL) {
        fclose(fh);
        return -1;
    }

    while (1) {
        size_t n;
        if (len == cap) {
            char *next;
            if (cap > (SIZE_MAX / 2U)) {
                free(buf);
                fclose(fh);
                return -1;
            }
            cap *= 2U;
            next = realloc(buf, cap + 1U);
            if (next == NULL) {
                free(buf);
                fclose(fh);
                return -1;
            }
            buf = next;
        }

        n = fread(buf + len, 1U, cap - len, fh);
        len += n;

        if (n == 0U) {
            if (ferror(fh)) {
                free(buf);
                fclose(fh);
                return -1;
            }
            break;
        }
    }

    fclose(fh);
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static char *dg_trim(char *text) {
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end >= text && isspace((unsigned char)*end)) {
        *end = '\0';
        if (end == text) {
            break;
        }
        end--;
    }

    return text;
}

static int dg_static_path_exists(const DgConfig *cfg, const char *path) {
    size_t i;
    for (i = 0; i < cfg->static_lease_count; i++) {
        if (strcmp(cfg->static_leases[i].usb_path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static int dg_static_ip_exists(const DgConfig *cfg, struct in_addr ip) {
    size_t i;
    for (i = 0; i < cfg->static_lease_count; i++) {
        if (cfg->static_leases[i].ip.s_addr == ip.s_addr) {
            return 1;
        }
    }
    return 0;
}

static int dg_parse_dns_list(char *value_text, DgConfig *cfg) {
    char *saveptr = NULL;
    char *item;

    cfg->dns_count = 0;
    item = strtok_r(value_text, ",", &saveptr);
    while (item != NULL) {
        struct in_addr dns_ip;
        char *trimmed = dg_trim(item);

        if (*trimmed != '\0') {
            if (cfg->dns_count >= DG_MAX_DNS_SERVERS) {
                return -1;
            }
            if (dg_parse_ipv4_string(trimmed, &dns_ip) != 0) {
                return -1;
            }
            cfg->dns[cfg->dns_count++] = dns_ip;
        }

        item = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}

static int dg_parse_static_lease(char *value_text, DgConfig *cfg) {
    char *sep;
    char *usb_path;
    char *ip_text;
    struct in_addr lease_ip;

    sep = strchr(value_text, '=');
    if (sep == NULL) {
        return -1;
    }

    *sep = '\0';
    usb_path = dg_trim(value_text);
    ip_text = dg_trim(sep + 1);

    if (*usb_path == '\0' || *ip_text == '\0') {
        return -1;
    }
    if (dg_parse_ipv4_string(ip_text, &lease_ip) != 0) {
        return -1;
    }
    if (dg_static_path_exists(cfg, usb_path) || dg_static_ip_exists(cfg, lease_ip)) {
        return -1;
    }
    if (cfg->static_lease_count >= DG_MAX_STATIC_LEASES) {
        return -1;
    }
    if (snprintf(cfg->static_leases[cfg->static_lease_count].usb_path,
                 sizeof(cfg->static_leases[cfg->static_lease_count].usb_path), "%s", usb_path) < 0) {
        return -1;
    }
    cfg->static_leases[cfg->static_lease_count].ip = lease_ip;
    cfg->static_lease_count++;

    return 0;
}

static int dg_custom_option_code_is_managed(uint32_t code) {
    return code == 1U || code == 3U || code == 6U || code == 51U || code == 53U || code == 54U;
}

static int dg_custom_option_code_exists(const DgConfig *cfg, uint8_t code) {
    size_t i;

    for (i = 0; i < cfg->custom_option_count; i++) {
        if (cfg->custom_options[i].code == code) {
            return 1;
        }
    }
    return 0;
}

static size_t dg_custom_options_encoded_len(const DgConfig *cfg) {
    size_t i;
    size_t total = 0U;

    for (i = 0; i < cfg->custom_option_count; i++) {
        total += 2U + cfg->custom_options[i].data_len;
    }
    return total;
}

static int dg_parse_custom_option(char *value_text, DgConfig *cfg) {
    char *sep;
    char *code_text;
    char *data_text;
    const char *cursor;
    uint32_t code;
    DgCustomOption option;

    sep = strchr(value_text, '=');
    if (sep == NULL) {
        return -1;
    }

    *sep = '\0';
    code_text = dg_trim(value_text);
    data_text = dg_trim(sep + 1);
    if (*code_text == '\0' || *data_text == '\0') {
        return -1;
    }

    if (dg_parse_u32_string(code_text, &code) != 0 || code == 0U || code >= 255U ||
        dg_custom_option_code_is_managed(code) || dg_custom_option_code_exists(cfg, (uint8_t)code) ||
        cfg->custom_option_count >= DG_MAX_CUSTOM_OPTIONS) {
        return -1;
    }

    memset(&option, 0, sizeof(option));
    option.code = (uint8_t)code;
    cursor = data_text;
    while (*cursor != '\0') {
        int high;
        int low;

        if (option.data_len >= DG_MAX_DHCP_OPTION_DATA_LEN || cursor[1] == '\0') {
            return -1;
        }
        high = dg_hex_nibble(cursor[0]);
        low = dg_hex_nibble(cursor[1]);
        if (high < 0 || low < 0) {
            return -1;
        }
        option.data[option.data_len++] = (uint8_t)((high << 4) | low);
        cursor += 2;

        if (*cursor == '\0') {
            break;
        }
        if (*cursor != ':' || cursor[1] == '\0') {
            return -1;
        }
        cursor++;
    }

    cfg->custom_options[cfg->custom_option_count++] = option;
    if (dg_custom_options_encoded_len(cfg) > DG_MAX_CUSTOM_OPTIONS_ENCODED_LEN) {
        cfg->custom_option_count--;
        return -1;
    }
    return 0;
}

static int dg_validate_collisions(const DgConfig *cfg) {
    size_t i;
    uint32_t pool_start = dg_ipv4_to_u32(cfg->pool.start);
    uint32_t pool_end = dg_ipv4_to_u32(cfg->pool.end);

    if (cfg->server_ip.s_addr == cfg->pool.start.s_addr || cfg->server_ip.s_addr == cfg->pool.end.s_addr) {
        return -1;
    }

    if (cfg->has_router && cfg->router.s_addr == cfg->server_ip.s_addr) {
        return -1;
    }

    for (i = 0; i < cfg->static_lease_count; i++) {
        uint32_t static_ip = dg_ipv4_to_u32(cfg->static_leases[i].ip);

        if (cfg->static_leases[i].ip.s_addr == cfg->server_ip.s_addr) {
            return -1;
        }
        if (cfg->has_router && cfg->static_leases[i].ip.s_addr == cfg->router.s_addr) {
            return -1;
        }
        if (static_ip >= pool_start && static_ip <= pool_end) {
            return -1;
        }
    }

    return 0;
}

static int dg_validate_network_ranges(const DgConfig *cfg) {
    size_t i;

    if (!dg_ipv4_in_subnet(cfg->pool.start, cfg->subnet_network, cfg->subnet_mask) ||
        !dg_ipv4_in_subnet(cfg->pool.end, cfg->subnet_network, cfg->subnet_mask) ||
        dg_ipv4_to_u32(cfg->pool.end) < dg_ipv4_to_u32(cfg->pool.start)) {
        return -1;
    }

    if (!dg_ipv4_in_subnet(cfg->server_ip, cfg->subnet_network, cfg->subnet_mask)) {
        return -1;
    }

    if (cfg->has_router && !dg_ipv4_in_subnet(cfg->router, cfg->subnet_network, cfg->subnet_mask)) {
        return -1;
    }

    for (i = 0; i < cfg->static_lease_count; i++) {
        if (!dg_ipv4_in_subnet(cfg->static_leases[i].ip, cfg->subnet_network, cfg->subnet_mask)) {
            return -1;
        }
    }

    return 0;
}

static int dg_parse_config_text(char *text, DgConfig *cfg) {
    char *saveptr = NULL;
    char *line;
    size_t line_no = 0;
    int have_subnet = 0;
    int have_pool_start = 0;
    int have_pool_end = 0;
    int have_server_ip = 0;

    if (text == NULL || cfg == NULL) {
        return -1;
    }

    cfg->has_router = 0;
    cfg->dns_count = 0;
    cfg->custom_option_count = 0;
    cfg->static_lease_count = 0;

    line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        char *trimmed_line;
        char *sep;
        char *key;
        char *value;

        line_no++;
        trimmed_line = dg_trim(line);

        if (*trimmed_line == '\0' || *trimmed_line == '#' || *trimmed_line == ';') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (trimmed_line[0] == '/' && trimmed_line[1] == '/') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        sep = strchr(trimmed_line, '=');
        if (sep == NULL) {
            dg_log(DG_LOG_ERROR, "config parse error at line %zu: expected key=value", line_no);
            return -1;
        }

        *sep = '\0';
        key = dg_trim(trimmed_line);
        value = dg_trim(sep + 1);

        if (*key == '\0' || *value == '\0') {
            dg_log(DG_LOG_ERROR, "config parse error at line %zu: empty key/value", line_no);
            return -1;
        }

        if (strcmp(key, "subnet") == 0) {
            if (dg_parse_subnet_cidr_string(value, &cfg->subnet_network, &cfg->subnet_mask, &cfg->subnet_prefix) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid subnet", line_no);
                return -1;
            }
            have_subnet = 1;
        } else if (strcmp(key, "server_ip") == 0) {
            if (dg_parse_ipv4_string(value, &cfg->server_ip) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid server_ip", line_no);
                return -1;
            }
            have_server_ip = 1;
        } else if (strcmp(key, "router") == 0) {
            if (dg_parse_ipv4_string(value, &cfg->router) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid router", line_no);
                return -1;
            }
            cfg->has_router = 1;
        } else if (strcmp(key, "lease_time") == 0) {
            if (dg_parse_u32_string(value, &cfg->pool.lease_seconds) != 0 || cfg->pool.lease_seconds == 0U) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid lease_time", line_no);
                return -1;
            }
        } else if (strcmp(key, "pool_start") == 0) {
            if (dg_parse_ipv4_string(value, &cfg->pool.start) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid pool_start", line_no);
                return -1;
            }
            have_pool_start = 1;
        } else if (strcmp(key, "pool_end") == 0) {
            if (dg_parse_ipv4_string(value, &cfg->pool.end) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid pool_end", line_no);
                return -1;
            }
            have_pool_end = 1;
        } else if (strcmp(key, "dns") == 0) {
            if (dg_parse_dns_list(value, cfg) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid dns list", line_no);
                return -1;
            }
        } else if (strcmp(key, "static_lease") == 0) {
            if (dg_parse_static_lease(value, cfg) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid static_lease entry", line_no);
                return -1;
            }
        } else if (strcmp(key, "custom_option") == 0) {
            if (dg_parse_custom_option(value, cfg) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid custom_option entry", line_no);
                return -1;
            }
        } else if (strcmp(key, "bridge") == 0) {
            if (dg_parse_ifname(value, cfg->bridge_name, sizeof(cfg->bridge_name)) != 0) {
                dg_log(DG_LOG_ERROR, "config parse error at line %zu: invalid bridge interface name", line_no);
                return -1;
            }
        } else {
            dg_log(DG_LOG_ERROR, "config parse error at line %zu: unknown key '%s'", line_no, key);
            return -1;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (!have_subnet || !have_pool_start || !have_pool_end) {
        dg_log(DG_LOG_ERROR, "config parse error: required keys are subnet, pool_start, pool_end");
        return -1;
    }

    if (!have_server_ip) {
        uint32_t network_u32 = dg_ipv4_to_u32(cfg->subnet_network);
        uint32_t first_host = network_u32;
        if (cfg->subnet_prefix < 32U) {
            first_host = network_u32 + 1U;
        }
        cfg->server_ip = dg_u32_to_ipv4(first_host);
    }

    if (dg_validate_network_ranges(cfg) != 0) {
        dg_log(DG_LOG_ERROR, "config validation failed: network values out of subnet/range");
        return -1;
    }
    if (dg_validate_collisions(cfg) != 0) {
        dg_log(DG_LOG_ERROR, "config validation failed: server/router/static/pool collision");
        return -1;
    }

    return 0;
}

void dg_config_set_defaults(DgConfig *out_cfg) {
    memset(out_cfg, 0, sizeof(*out_cfg));

    dg_parse_ipv4_string("192.168.8.1", &out_cfg->server_ip);
    dg_parse_ipv4_string("192.168.8.0", &out_cfg->subnet_network);
    dg_parse_ipv4_string("255.255.255.0", &out_cfg->subnet_mask);
    out_cfg->subnet_prefix = 24;
    out_cfg->has_router = 0;
    dg_parse_ipv4_string("192.168.8.100", &out_cfg->pool.start);
    dg_parse_ipv4_string("192.168.8.200", &out_cfg->pool.end);
    out_cfg->pool.lease_seconds = 86400;
    out_cfg->dns_count = 0;
    out_cfg->custom_option_count = 0;
    out_cfg->static_lease_count = 0;
    strcpy(out_cfg->bridge_name, "gadget0");
}

int dg_config_load(const char *path, DgConfig *out_cfg) {
    char *text = NULL;
    size_t text_len = 0;
    size_t i;

    if (path == NULL || out_cfg == NULL) {
        dg_log(DG_LOG_ERROR, "config load failed: invalid arguments");
        return -1;
    }

    dg_config_set_defaults(out_cfg);
    strncpy(out_cfg->config_path, path, sizeof(out_cfg->config_path) - 1U);
    out_cfg->config_path[sizeof(out_cfg->config_path) - 1U] = '\0';

    if (dg_read_file_bytes(path, &text, &text_len) != 0) {
        dg_log(DG_LOG_ERROR, "config open failed for %s: %s", path, strerror(errno));
        return -1;
    }

    if (text_len == 0U) {
        dg_log(DG_LOG_ERROR, "config file is empty: %s", path);
        free(text);
        return -1;
    }

    if (dg_parse_config_text(text, out_cfg) != 0) {
        dg_log(DG_LOG_ERROR, "config parse/validation failed: %s", path);
        free(text);
        return -1;
    }

    free(text);
    dg_log(DG_LOG_INFO, "config loaded: bridge=%s lease_time=%u static_leases=%zu dns=%zu custom_options=%zu",
           out_cfg->bridge_name, out_cfg->pool.lease_seconds, out_cfg->static_lease_count, out_cfg->dns_count,
           out_cfg->custom_option_count);

    for (i = 0; i < out_cfg->static_lease_count; i++) {
        char ip_text[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &out_cfg->static_leases[i].ip, ip_text, sizeof(ip_text)) == NULL) {
            strncpy(ip_text, "<invalid>", sizeof(ip_text) - 1U);
            ip_text[sizeof(ip_text) - 1U] = '\0';
        }
        dg_log(DG_LOG_DEBUG, "static lease configured usb_path=%s ip=%s", out_cfg->static_leases[i].usb_path,
               ip_text);
    }

    return 0;
}
