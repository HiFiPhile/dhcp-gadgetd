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

int main(void) {
    test_custom_options_load();
    test_invalid_custom_options_fail();
    test_custom_option_size_limits();
    test_bridge_name();
    puts("config tests passed");
    return 0;
}
