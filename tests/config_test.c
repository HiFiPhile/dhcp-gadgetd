#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static const char *base_config =
    "subnet=192.168.8.0/24\n"
    "pool_start=192.168.8.100\n"
    "pool_end=192.168.8.200\n";

static int load_text(const char *suffix, DgConfig *cfg) {
    char path[] = "/tmp/dhcp-gadget-config-test-XXXXXX";
    int fd;
    size_t base_len = strlen(base_config);
    size_t suffix_len = strlen(suffix);
    ssize_t written;
    int rc;

    fd = mkstemp(path);
    assert(fd >= 0);

    written = write(fd, base_config, base_len);
    assert(written == (ssize_t)base_len);
    written = write(fd, suffix, suffix_len);
    assert(written == (ssize_t)suffix_len);
    assert(close(fd) == 0);

    rc = dg_config_load(path, cfg);
    assert(unlink(path) == 0);
    return rc;
}

/* Loads a config expected to fail and reports whether the error log contains needle. */
static int load_fails_logging(const char *suffix, const char *needle) {
    FILE *log = tmpfile();
    int saved_stderr = dup(STDERR_FILENO);
    char text[4096];
    size_t len;
    DgConfig cfg;

    assert(log != NULL && saved_stderr >= 0);
    fflush(stderr);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    assert(load_text(suffix, &cfg) != 0);
    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    assert(close(saved_stderr) == 0);

    rewind(log);
    len = fread(text, 1U, sizeof(text) - 1U, log);
    text[len] = '\0';
    assert(fclose(log) == 0);
    return strstr(text, needle) != NULL;
}

static void test_custom_options_load(void) {
    DgConfig cfg;

    assert(load_text("custom_option=42=c0:a8:08:02\ncustom_option=224=01:02:fF\n", &cfg) == 0);
    assert(cfg.custom_option_count == 2U);
    assert(cfg.custom_options[0].code == 42U);
    assert(cfg.custom_options[0].data_len == 4U);
    assert(cfg.custom_options[0].data[0] == 0xc0U);
    assert(cfg.custom_options[0].data[1] == 0xa8U);
    assert(cfg.custom_options[0].data[2] == 0x08U);
    assert(cfg.custom_options[0].data[3] == 0x02U);
    assert(cfg.custom_options[1].code == 224U);
    assert(cfg.custom_options[1].data_len == 3U);
    assert(cfg.custom_options[1].data[2] == 0xffU);
}

static void test_invalid_custom_options_fail(void) {
    DgConfig cfg;

    assert(load_text("custom_option=0=01\n", &cfg) != 0);
    assert(load_text("custom_option=53=01\n", &cfg) != 0);
    assert(load_text("custom_option=42=01\ncustom_option=42=02\n", &cfg) != 0);
    assert(load_text("custom_option=42=0\n", &cfg) != 0);
    assert(load_text("custom_option=42=01:xy\n", &cfg) != 0);
    assert(load_text("custom_option=42=0102\n", &cfg) != 0);
}

static void test_custom_option_size_limits(void) {
    char suffix[1024];
    DgConfig cfg;
    size_t off;
    size_t i;
    int n;

    off = (size_t)snprintf(suffix, sizeof(suffix), "custom_option=42=");
    assert(off < sizeof(suffix));
    for (i = 0; i < DG_MAX_DHCP_OPTION_DATA_LEN; i++) {
        const char *byte_text = i + 1U < DG_MAX_DHCP_OPTION_DATA_LEN ? "aa:" : "aa\n";
        n = snprintf(suffix + off, sizeof(suffix) - off, "%s", byte_text);
        assert(n > 0 && (size_t)n < sizeof(suffix) - off);
        off += (size_t)n;
    }

    assert(load_text(suffix, &cfg) == 0);
    assert(cfg.custom_options[0].data_len == DG_MAX_DHCP_OPTION_DATA_LEN);

    n = snprintf(suffix + off, sizeof(suffix) - off, "custom_option=43=00:00:00:00\n");
    assert(n > 0 && (size_t)n < sizeof(suffix) - off);
    assert(load_text(suffix, &cfg) != 0);
}

static void test_bridge_name(void) {
    DgConfig cfg;

    assert(load_text("", &cfg) == 0);
    assert(strcmp(cfg.bridge_name, "gadget0") == 0);
    assert(load_text("bridge=br-gadget\n", &cfg) == 0);
    assert(strcmp(cfg.bridge_name, "br-gadget") == 0);
    assert(load_text("bridge=0123456789abcde\n", &cfg) == 0);
    assert(load_text("bridge=0123456789abcdef\n", &cfg) != 0);
    assert(load_text("bridge=a/b\n", &cfg) != 0);
    assert(load_text("bridge=a:b\n", &cfg) != 0);
    assert(load_text("bridge=a b\n", &cfg) != 0);
    assert(load_text("bridge=..\n", &cfg) != 0);
}

static void test_drivers(void) {
    DgConfig cfg;

    assert(load_text("", &cfg) == 0);
    assert(cfg.driver_count == 2);
    assert(strcmp(cfg.drivers[0], "cdc_ether") == 0 && strcmp(cfg.drivers[1], "cdc_eem") == 0);
    assert(load_text("drivers=cdc_ncm, rndis_host\n", &cfg) == 0);
    assert(cfg.driver_count == 2);
    assert(strcmp(cfg.drivers[0], "cdc_ncm") == 0 && strcmp(cfg.drivers[1], "rndis_host") == 0);
    assert(load_text("drivers=cdc_ether\ndrivers=ax88179_178a\n", &cfg) == 0);
    assert(cfg.driver_count == 1 && strcmp(cfg.drivers[0], "ax88179_178a") == 0);
    assert(load_text("drivers=a,b,c,d,e,f,g,h\n", &cfg) == 0 && cfg.driver_count == DG_MAX_DRIVERS);
    assert(load_text("drivers=a,b,c,d,e,f,g,h,i\n", &cfg) != 0);
    assert(load_text("drivers=0123456789012345678901234567890\n", &cfg) == 0);
    assert(load_text("drivers=01234567890123456789012345678901\n", &cfg) != 0);
    assert(load_text("drivers=cdc_ether,cdc_ether\n", &cfg) != 0);
    assert(load_text("drivers=cdc_*\n", &cfg) != 0);
    assert(load_text("drivers=cdc_ether|cdc_eem\n", &cfg) != 0);
    assert(load_text("drivers=,\n", &cfg) != 0);
}

static void test_router(void) {
    DgConfig cfg;

    /* This host as the gadgets' router, the usual case with NAT on the host. */
    assert(load_text("router=192.168.8.1\n", &cfg) == 0);
    assert(cfg.has_router);
    assert(cfg.router.s_addr == cfg.server_ip.s_addr);
    assert(load_text("server_ip=192.168.8.2\nrouter=192.168.8.254\n", &cfg) == 0);
    assert(load_text("router=192.168.9.1\n", &cfg) != 0);
    assert(load_text("router=192.168.8.20\nstatic_lease=1-1:1.0=192.168.8.20\n", &cfg) != 0);
    /* The pool would hand it out. */
    assert(load_text("router=192.168.8.150\n", &cfg) != 0);
    assert(load_text("router=192.168.8.200\n", &cfg) != 0);
}

static void test_address_rules(void) {
    DgConfig cfg;

    /* Anywhere in the pool, not only at its ends. */
    assert(load_text("server_ip=192.168.8.100\n", &cfg) != 0);
    assert(load_text("server_ip=192.168.8.150\n", &cfg) != 0);
    assert(load_text("server_ip=192.168.8.201\n", &cfg) == 0);

    /* Network and broadcast addresses are not host addresses. */
    assert(load_text("server_ip=192.168.8.0\n", &cfg) != 0);
    assert(load_text("server_ip=192.168.8.255\n", &cfg) != 0);
    assert(load_text("router=192.168.8.255\n", &cfg) != 0);
    assert(load_text("static_lease=1-1:1.0=192.168.8.255\n", &cfg) != 0);
    assert(load_text("pool_end=192.168.8.255\n", &cfg) != 0);
    assert(load_text("pool_end=192.168.8.254\n", &cfg) == 0);
    assert(load_text("server_ip=192.168.8.250\npool_start=192.168.8.0\n", &cfg) != 0);
    assert(load_text("server_ip=192.168.8.250\npool_start=192.168.8.1\n", &cfg) == 0);

    /* /31 has neither (RFC 3021), so both addresses are usable. */
    assert(load_text("subnet=10.0.0.0/31\nserver_ip=10.0.0.0\npool_start=10.0.0.1\npool_end=10.0.0.1\n", &cfg) == 0);
}

static void test_subnet_prefix(void) {
    DgConfig cfg;

    assert(load_text("subnet=192.168.8.0/24\n", &cfg) == 0 && cfg.subnet_prefix == 24U);
    assert(load_text("subnet=192.168.8.0/\n", &cfg) != 0);
    assert(load_text("subnet=192.168.8.0/+24\n", &cfg) != 0);
    assert(load_text("subnet=192.168.8.0/ 24\n", &cfg) != 0);
    assert(load_text("subnet=192.168.8.0/33\n", &cfg) != 0);
}

static void test_static_lease_path_length(void) {
    char suffix[DG_MAX_PATH_LEN + 64];
    char path[DG_MAX_PATH_LEN + 1];
    DgConfig cfg;

    memset(path, '1', sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    assert(snprintf(suffix, sizeof(suffix), "static_lease=%s=192.168.8.20\n", path + 1) < (int)sizeof(suffix));
    assert(load_text(suffix, &cfg) == 0);
    assert(strlen(cfg.static_leases[0].usb_path) == DG_MAX_PATH_LEN - 1U);
    /* One more character used to be silently cut off, so the lease could never match. */
    assert(snprintf(suffix, sizeof(suffix), "static_lease=%s=192.168.8.20\n", path) < (int)sizeof(suffix));
    assert(load_text(suffix, &cfg) != 0);
}

static void test_error_line_numbers(void) {
    /* base_config is lines 1-3; empty, blank and comment lines still count. */
    assert(load_fails_logging("\n\nbogus=1\n", "at line 6:"));
    assert(load_fails_logging("\n  \n# comment\n\nlease_time=0\n", "at line 8:"));
}

int main(void) {
    test_custom_options_load();
    test_router();
    test_address_rules();
    test_subnet_prefix();
    test_static_lease_path_length();
    test_error_line_numbers();
    test_invalid_custom_options_fail();
    test_custom_option_size_limits();
    test_bridge_name();
    test_drivers();
    puts("config tests passed");
    return 0;
}
