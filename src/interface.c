#include "interface.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define DG_MAX_INTERFACES 128

static DgInterfaceEntry g_entries[DG_MAX_INTERFACES];
static size_t g_entry_count = 0;

static int dg_find_entry_index_by_name(const char *ifname) {
    size_t i;
    for (i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].ifname, ifname) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int dg_find_entry_index_by_ifindex(int ifindex) {
    size_t i;
    for (i = 0; i < g_entry_count; i++) {
        if (g_entries[i].ifindex == ifindex) {
            return (int)i;
        }
    }
    return -1;
}

static void dg_remove_entry_at(size_t idx) {
    size_t i;
    if (idx >= g_entry_count) {
        return;
    }
    for (i = idx; i + 1U < g_entry_count; i++) {
        g_entries[i] = g_entries[i + 1U];
    }
    g_entry_count--;
}

static int dg_readlink_into(const char *path, char *out, size_t out_size) {
    ssize_t n;
    if (path == NULL || out == NULL || out_size == 0U) {
        return -1;
    }
    n = readlink(path, out, out_size - 1U);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static int dg_read_text_file(const char *path, char *out, size_t out_size) {
    FILE *fh;
    char *newline;

    if (path == NULL || out == NULL || out_size == 0U) {
        return -1;
    }

    fh = fopen(path, "r");
    if (fh == NULL) {
        return -1;
    }

    if (fgets(out, (int)out_size, fh) == NULL) {
        fclose(fh);
        return -1;
    }
    fclose(fh);

    newline = strchr(out, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    return 0;
}

static const char *dg_path_basename(const char *path) {
    const char *slash;
    if (path == NULL) {
        return "";
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return path;
    }
    return slash + 1;
}

static int dg_is_supported_driver(const char *driver_name) {
    return strcmp(driver_name, "cdc_ether") == 0 || strcmp(driver_name, "cdc_eem") == 0;
}

static int dg_extract_usb_path(const char *device_link, char *out_usb_path, size_t out_size) {
    char tmp[PATH_MAX];
    char *tok;
    char *saveptr = NULL;

    if (device_link == NULL || out_usb_path == NULL || out_size == 0U) {
        return -1;
    }

    if (strlen(device_link) >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, device_link);

    tok = strtok_r(tmp, "/", &saveptr);
    while (tok != NULL) {
        if (strchr(tok, ':') != NULL && strchr(tok, '-') != NULL) {
            size_t len = strlen(tok);
            if (len + 1U > out_size) {
                return -1;
            }
            memcpy(out_usb_path, tok, len + 1U);
            return 0;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    return -1;
}

static int dg_fill_interface_entry(const char *ifname, DgInterfaceEntry *entry) {
    char path[PATH_MAX];
    char driver_link[PATH_MAX];
    char device_link[PATH_MAX];
    char mac_text[32];
    int n;
    unsigned int ifindex;

    n = snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", ifname);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    if (dg_readlink_into(path, driver_link, sizeof(driver_link)) != 0) {
        return -1;
    }
    if (!dg_is_supported_driver(dg_path_basename(driver_link))) {
        return -1;
    }

    n = snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    if (dg_readlink_into(path, device_link, sizeof(device_link)) != 0) {
        return -1;
    }
    if (dg_extract_usb_path(device_link, entry->usb_path, sizeof(entry->usb_path)) != 0) {
        return -1;
    }

    n = snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    if (dg_read_text_file(path, mac_text, sizeof(mac_text)) != 0) {
        return -1;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0U) {
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->ifindex = (int)ifindex;
    if (snprintf(entry->ifname, sizeof(entry->ifname), "%s", ifname) < 0) {
        return -1;
    }
    if (snprintf(entry->mac, sizeof(entry->mac), "%s", mac_text) < 0) {
        return -1;
    }
    return 0;
}

static int dg_entries_equal(const DgInterfaceEntry *a, const DgInterfaceEntry *b) {
    return a->ifindex == b->ifindex && strcmp(a->ifname, b->ifname) == 0 && strcmp(a->usb_path, b->usb_path) == 0 &&
           strcmp(a->mac, b->mac) == 0;
}

int dg_interface_refresh(void) {
    DIR *dir;
    struct dirent *de;
    DgInterfaceEntry next[DG_MAX_INTERFACES];
    size_t next_count = 0;
    int changed = 0;
    size_t i;

    dir = opendir("/sys/class/net");
    if (dir == NULL) {
        dg_log(DG_LOG_WARN, "interface refresh failed: %s", strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (next_count >= DG_MAX_INTERFACES) {
            break;
        }
        if (dg_fill_interface_entry(de->d_name, &next[next_count]) == 0) {
            next_count++;
        }
    }
    closedir(dir);

    if (next_count != g_entry_count) {
        changed = 1;
    } else {
        for (i = 0; i < next_count; i++) {
            if (!dg_entries_equal(&next[i], &g_entries[i])) {
                changed = 1;
                break;
            }
        }
    }

    memcpy(g_entries, next, sizeof(next));
    g_entry_count = next_count;

    if (changed) {
        dg_log(DG_LOG_INFO, "interface cache updated: count=%zu", g_entry_count);
    } else {
        dg_log(DG_LOG_DEBUG, "interface refresh tick");
    }

    return 0;
}

int dg_interface_upsert(const char *ifname) {
    DgInterfaceEntry entry;
    int by_index;
    int by_name;

    if (ifname == NULL || ifname[0] == '\0') {
        return -1;
    }
    if (dg_fill_interface_entry(ifname, &entry) != 0) {
        return -1;
    }

    by_index = dg_find_entry_index_by_ifindex(entry.ifindex);
    if (by_index >= 0) {
        g_entries[(size_t)by_index] = entry;
        return 0;
    }

    by_name = dg_find_entry_index_by_name(ifname);
    if (by_name >= 0) {
        g_entries[(size_t)by_name] = entry;
        return 0;
    }

    if (g_entry_count >= DG_MAX_INTERFACES) {
        return -1;
    }
    g_entries[g_entry_count++] = entry;
    return 0;
}

int dg_interface_remove(const char *ifname) {
    int idx;
    if (ifname == NULL || ifname[0] == '\0') {
        return -1;
    }
    idx = dg_find_entry_index_by_name(ifname);
    if (idx < 0) {
        return -1;
    }
    dg_remove_entry_at((size_t)idx);
    return 0;
}

size_t dg_interface_count(void) {
    return g_entry_count;
}

const DgInterfaceEntry *dg_interface_get(size_t idx) {
    if (idx >= g_entry_count) {
        return NULL;
    }
    return &g_entries[idx];
}

const DgInterfaceEntry *dg_interface_by_ifindex(int ifindex) {
    size_t i;
    for (i = 0; i < g_entry_count; i++) {
        if (g_entries[i].ifindex == ifindex) {
            return &g_entries[i];
        }
    }
    return NULL;
}

const DgInterfaceEntry *dg_interface_by_name(const char *ifname) {
    int idx;
    if (ifname == NULL) {
        return NULL;
    }
    idx = dg_find_entry_index_by_name(ifname);
    if (idx < 0) {
        return NULL;
    }
    return &g_entries[(size_t)idx];
}