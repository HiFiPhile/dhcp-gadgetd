#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsmn.h"
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

static int dg_token_to_text(const char *json, const jsmntok_t *tok, char *out, size_t out_size) {
    size_t len;

    if (json == NULL || tok == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    if (tok->start < 0 || tok->end < tok->start) {
        return -1;
    }

    len = (size_t)(tok->end - tok->start);
    if (len + 1 > out_size) {
        return -1;
    }

    memcpy(out, json + tok->start, len);
    out[len] = '\0';
    return 0;
}

static int dg_token_equals(const char *json, const jsmntok_t *tok, const char *expected) {
    size_t expected_len;
    size_t tok_len;

    if (json == NULL || tok == NULL || expected == NULL) {
        return 0;
    }
    if (tok->start < 0 || tok->end < tok->start) {
        return 0;
    }

    expected_len = strlen(expected);
    tok_len = (size_t)(tok->end - tok->start);
    if (tok_len != expected_len) {
        return 0;
    }

    return memcmp(json + tok->start, expected, tok_len) == 0;
}

static int dg_find_object_key(const char *json, const jsmntok_t *tokens, int token_count, int object_idx,
                              const char *key) {
    int i;

    if (json == NULL || tokens == NULL || key == NULL || object_idx < 0 || object_idx >= token_count) {
        return -1;
    }
    if (tokens[object_idx].type != JSMN_OBJECT) {
        return -1;
    }

    for (i = object_idx + 1; i + 1 < token_count; i++) {
        if (tokens[i].parent != object_idx || tokens[i].type != JSMN_STRING) {
            continue;
        }
        if (tokens[i + 1].parent != object_idx) {
            continue;
        }
        if (dg_token_equals(json, &tokens[i], key)) {
            return i + 1;
        }
    }

    return -1;
}

static int dg_parse_u32_token(const char *json, const jsmntok_t *tok, uint32_t *out_value) {
    char tmp[32];
    char *endptr;
    unsigned long value;

    if (json == NULL || tok == NULL || out_value == NULL) {
        return -1;
    }
    if (tok->type != JSMN_PRIMITIVE && tok->type != JSMN_STRING) {
        return -1;
    }
    if (dg_token_to_text(json, tok, tmp, sizeof(tmp)) != 0) {
        return -1;
    }

    errno = 0;
    value = strtoul(tmp, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || value > UINT_MAX) {
        return -1;
    }

    *out_value = (uint32_t)value;
    return 0;
}

static int dg_parse_ipv4_token(const char *json, const jsmntok_t *tok, struct in_addr *out_addr) {
    char tmp[32];

    if (json == NULL || tok == NULL || out_addr == NULL || tok->type != JSMN_STRING) {
        return -1;
    }
    if (dg_token_to_text(json, tok, tmp, sizeof(tmp)) != 0) {
        return -1;
    }

    return dg_parse_ipv4_string(tmp, out_addr);
}

static int dg_parse_subnet_cidr_token(const char *json, const jsmntok_t *tok, struct in_addr *out_network,
                                      struct in_addr *out_mask, uint8_t *out_prefix) {
    char tmp[48];
    char *slash;
    char *endptr;
    unsigned long prefix_num;
    struct in_addr ip;
    uint32_t mask_u32;
    uint32_t network_u32;

    if (json == NULL || tok == NULL || out_network == NULL || out_mask == NULL || out_prefix == NULL) {
        return -1;
    }
    if (tok->type != JSMN_STRING) {
        return -1;
    }
    if (dg_token_to_text(json, tok, tmp, sizeof(tmp)) != 0) {
        return -1;
    }

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

    buf = malloc(cap + 1);
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

static int dg_parse_dns_array(const char *json, const jsmntok_t *tokens, int token_count, int dns_idx,
                              DgConfig *cfg) {
    int i;

    (void)token_count;

    if (tokens[dns_idx].type != JSMN_ARRAY) {
        return -1;
    }

    cfg->dns_count = 0;
    for (i = dns_idx + 1; i < token_count; i++) {
        struct in_addr dns_ip;
        if (tokens[i].parent != dns_idx) {
            continue;
        }
        if (tokens[i].type != JSMN_STRING) {
            return -1;
        }
        if (cfg->dns_count >= DG_MAX_DNS_SERVERS) {
            return -1;
        }
        if (dg_parse_ipv4_token(json, &tokens[i], &dns_ip) != 0) {
            return -1;
        }
        cfg->dns[cfg->dns_count++] = dns_ip;
    }
    return 0;
}

static int dg_parse_static_leases_object(const char *json, const jsmntok_t *tokens, int token_count,
                                         int static_idx, DgConfig *cfg) {
    int i;

    if (tokens[static_idx].type != JSMN_OBJECT) {
        return -1;
    }

    cfg->static_lease_count = 0;

    for (i = static_idx + 1; i + 1 < token_count;) {
        char key_buf[DG_MAX_PATH_LEN];
        struct in_addr lease_ip;
        int value_idx;

        if (tokens[i].parent != static_idx) {
            i++;
            continue;
        }
        if (tokens[i].type != JSMN_STRING) {
            return -1;
        }
        value_idx = i + 1;
        if (tokens[value_idx].parent != static_idx || tokens[value_idx].type != JSMN_STRING) {
            return -1;
        }

        if (dg_token_to_text(json, &tokens[i], key_buf, sizeof(key_buf)) != 0 || key_buf[0] == '\0') {
            return -1;
        }
        if (dg_parse_ipv4_token(json, &tokens[value_idx], &lease_ip) != 0) {
            return -1;
        }
        if (!dg_ipv4_in_subnet(lease_ip, cfg->subnet_network, cfg->subnet_mask)) {
            return -1;
        }
        if (dg_static_path_exists(cfg, key_buf) || dg_static_ip_exists(cfg, lease_ip)) {
            return -1;
        }
        if (cfg->static_lease_count >= DG_MAX_STATIC_LEASES) {
            return -1;
        }

        {
            size_t dst_cap = sizeof(cfg->static_leases[cfg->static_lease_count].usb_path);
            size_t key_len = strlen(key_buf);
            if (key_len + 1U > dst_cap) {
                return -1;
            }
            memcpy(cfg->static_leases[cfg->static_lease_count].usb_path, key_buf, key_len + 1U);
        }
        cfg->static_leases[cfg->static_lease_count].ip = lease_ip;
        cfg->static_lease_count++;

        i = value_idx + 1;
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

static int dg_parse_config_json(const char *json, size_t json_len, DgConfig *out_cfg) {
    jsmntok_t *tokens = NULL;
    size_t token_cap = 256;
    int token_count;
    int root_idx = 0;
    int subnet_idx;
    int pool_idx;
    int start_idx;
    int end_idx;
    int server_ip_idx;
    int router_idx;
    int lease_time_idx;
    int dns_idx;
    int static_idx;
    jsmn_parser parser;

    while (1) {
        jsmn_init(&parser);
        tokens = calloc(token_cap, sizeof(*tokens));
        if (tokens == NULL) {
            return -1;
        }

        token_count = jsmn_parse(&parser, json, json_len, tokens, (unsigned int)token_cap);
        if (token_count == JSMN_ERROR_NOMEM) {
            free(tokens);
            tokens = NULL;
            if (token_cap > (SIZE_MAX / 2U)) {
                return -1;
            }
            token_cap *= 2U;
            continue;
        }
        break;
    }

    if (token_count <= 0 || tokens[root_idx].type != JSMN_OBJECT) {
        free(tokens);
        return -1;
    }

    subnet_idx = dg_find_object_key(json, tokens, token_count, root_idx, "subnet");
    pool_idx = dg_find_object_key(json, tokens, token_count, root_idx, "pool");
    if (subnet_idx < 0 || pool_idx < 0) {
        free(tokens);
        return -1;
    }
    if (dg_parse_subnet_cidr_token(json, &tokens[subnet_idx], &out_cfg->subnet_network, &out_cfg->subnet_mask,
                                   &out_cfg->subnet_prefix) != 0) {
        free(tokens);
        return -1;
    }

    if (tokens[pool_idx].type != JSMN_OBJECT) {
        free(tokens);
        return -1;
    }
    start_idx = dg_find_object_key(json, tokens, token_count, pool_idx, "start");
    end_idx = dg_find_object_key(json, tokens, token_count, pool_idx, "end");
    if (start_idx < 0 || end_idx < 0) {
        free(tokens);
        return -1;
    }
    if (dg_parse_ipv4_token(json, &tokens[start_idx], &out_cfg->pool.start) != 0 ||
        dg_parse_ipv4_token(json, &tokens[end_idx], &out_cfg->pool.end) != 0) {
        free(tokens);
        return -1;
    }

    if (!dg_ipv4_in_subnet(out_cfg->pool.start, out_cfg->subnet_network, out_cfg->subnet_mask) ||
        !dg_ipv4_in_subnet(out_cfg->pool.end, out_cfg->subnet_network, out_cfg->subnet_mask) ||
        dg_ipv4_to_u32(out_cfg->pool.end) < dg_ipv4_to_u32(out_cfg->pool.start)) {
        free(tokens);
        return -1;
    }

    server_ip_idx = dg_find_object_key(json, tokens, token_count, root_idx, "server_ip");
    if (server_ip_idx >= 0) {
        if (dg_parse_ipv4_token(json, &tokens[server_ip_idx], &out_cfg->server_ip) != 0) {
            free(tokens);
            return -1;
        }
    } else {
        uint32_t network_u32 = dg_ipv4_to_u32(out_cfg->subnet_network);
        uint32_t first_host = network_u32;
        if (out_cfg->subnet_prefix < 32U) {
            first_host = network_u32 + 1U;
        }
        out_cfg->server_ip = dg_u32_to_ipv4(first_host);
    }

    if (!dg_ipv4_in_subnet(out_cfg->server_ip, out_cfg->subnet_network, out_cfg->subnet_mask)) {
        free(tokens);
        return -1;
    }

    out_cfg->has_router = 0;
    router_idx = dg_find_object_key(json, tokens, token_count, root_idx, "router");
    if (router_idx >= 0) {
        if (dg_parse_ipv4_token(json, &tokens[router_idx], &out_cfg->router) != 0 ||
            !dg_ipv4_in_subnet(out_cfg->router, out_cfg->subnet_network, out_cfg->subnet_mask)) {
            free(tokens);
            return -1;
        }
        out_cfg->has_router = 1;
    }

    lease_time_idx = dg_find_object_key(json, tokens, token_count, root_idx, "lease_time");
    if (lease_time_idx >= 0) {
        if (dg_parse_u32_token(json, &tokens[lease_time_idx], &out_cfg->pool.lease_seconds) != 0 ||
            out_cfg->pool.lease_seconds == 0U) {
            free(tokens);
            return -1;
        }
    }

    out_cfg->dns_count = 0;
    dns_idx = dg_find_object_key(json, tokens, token_count, root_idx, "dns");
    if (dns_idx >= 0 && dg_parse_dns_array(json, tokens, token_count, dns_idx, out_cfg) != 0) {
        free(tokens);
        return -1;
    }

    static_idx = dg_find_object_key(json, tokens, token_count, root_idx, "static_leases");
    if (static_idx >= 0) {
        if (dg_parse_static_leases_object(json, tokens, token_count, static_idx, out_cfg) != 0) {
            free(tokens);
            return -1;
        }
    }

    if (dg_validate_collisions(out_cfg) != 0) {
        free(tokens);
        return -1;
    }

    free(tokens);
    return 0;
}

void dg_config_set_defaults(DgConfig *out_cfg) {
    memset(out_cfg, 0, sizeof(*out_cfg));

    dg_parse_ipv4_string("192.168.7.1", &out_cfg->server_ip);
    dg_parse_ipv4_string("192.168.7.0", &out_cfg->subnet_network);
    dg_parse_ipv4_string("255.255.255.0", &out_cfg->subnet_mask);
    out_cfg->subnet_prefix = 24;
    out_cfg->has_router = 0;
    dg_parse_ipv4_string("192.168.7.100", &out_cfg->pool.start);
    dg_parse_ipv4_string("192.168.7.200", &out_cfg->pool.end);
    out_cfg->pool.lease_seconds = 86400;
    out_cfg->dns_count = 0;
    out_cfg->static_lease_count = 0;
}

int dg_config_load(const char *path, DgConfig *out_cfg) {
    char *json = NULL;
    size_t json_len = 0;

    if (path == NULL || out_cfg == NULL) {
        dg_log(DG_LOG_ERROR, "config load failed: invalid arguments");
        return -1;
    }

    dg_config_set_defaults(out_cfg);
    strncpy(out_cfg->config_path, path, sizeof(out_cfg->config_path) - 1U);
    out_cfg->config_path[sizeof(out_cfg->config_path) - 1U] = '\0';

    if (dg_read_file_bytes(path, &json, &json_len) != 0) {
        dg_log(DG_LOG_ERROR, "config open failed for %s: %s", path, strerror(errno));
        return -1;
    }

    if (json_len == 0U) {
        dg_log(DG_LOG_ERROR, "config file is empty: %s", path);
        free(json);
        return -1;
    }

    if (dg_parse_config_json(json, json_len, out_cfg) != 0) {
        dg_log(DG_LOG_ERROR, "config parse/validation failed: %s", path);
        free(json);
        return -1;
    }

    free(json);
    dg_log(DG_LOG_INFO, "config loaded: lease_time=%u static_leases=%zu dns=%zu", out_cfg->pool.lease_seconds,
           out_cfg->static_lease_count, out_cfg->dns_count);
    return 0;
}