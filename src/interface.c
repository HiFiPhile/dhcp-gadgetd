#include "interface.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

#define DG_MAX_INTERFACES 128

static DgInterfaceEntry g_entries[DG_MAX_INTERFACES];
static size_t g_entry_count = 0;

static void dg_close_entry_socket(DgInterfaceEntry *entry) {
    if (entry->sock_fd >= 0) {
        close(entry->sock_fd);
    }
    entry->sock_fd = -1;
}

static int dg_open_bound_socket(int ifindex) {
    int fd;
    int flags;
    struct sockaddr_ll bind_addr;

    fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

#ifdef PACKET_IGNORE_OUTGOING
    {
        int one = 1;
        (void)setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, (socklen_t)sizeof(one));
    }
#endif

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = AF_PACKET;
    /* Need to be ETH_P_ALL, strange */
    bind_addr.sll_protocol = htons(ETH_P_ALL);
    bind_addr.sll_ifindex = ifindex;

    if (bind(fd, (const struct sockaddr *)&bind_addr, (socklen_t)sizeof(bind_addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int dg_try_bind_entry(DgInterfaceEntry *entry) {
    int fd;

    if (entry->ifindex <= 0) {
        return -1;
    }
    if (entry->sock_fd >= 0) {
        return 0;
    }

    fd = dg_open_bound_socket(entry->ifindex);
    if (fd < 0) {
        if (errno == EPERM || errno == EACCES) {
            dg_log(DG_LOG_WARN, "interface bind skipped if=%s idx=%d (no CAP_NET_RAW)", entry->ifname, entry->ifindex);
            return 0;
        }
        dg_log(DG_LOG_WARN, "interface bind failed if=%s idx=%d err=%s", entry->ifname, entry->ifindex, strerror(errno));
        return -1;
    }

    entry->sock_fd = fd;
    return 0;
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
    dg_close_entry_socket(&g_entries[idx]);
    for (i = idx; i + 1U < g_entry_count; i++) {
        g_entries[i] = g_entries[i + 1U];
    }
    g_entry_count--;
}

static int dg_readlink_into(const char *path, char *out, size_t out_size) {
    ssize_t n;
    n = readlink(path, out, out_size - 1U);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static const char *dg_path_basename(const char *path) {
    const char *slash;
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

static int dg_resolve_driver_name(const char *ifname,
                                  const char *driver_hint,
                                  char *out_driver,
                                  size_t out_driver_size) {
    char path[PATH_MAX];
    char driver_link[PATH_MAX];
    const char *driver_name;
    int n;

    if (driver_hint != NULL && driver_hint[0] != '\0') {
        driver_name = driver_hint;
    } else {
        n = snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", ifname);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=driver-path-format", ifname);
            return -1;
        }
        if (dg_readlink_into(path, driver_link, sizeof(driver_link)) != 0) {
            dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=driver-readlink err=%s", ifname, strerror(errno));
            return -1;
        }
        driver_name = dg_path_basename(driver_link);
    }

    if (!dg_is_supported_driver(driver_name)) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=unsupported-driver driver=%s", ifname, driver_name);
        return -1;
    }

    if (snprintf(out_driver, out_driver_size, "%s", driver_name) < 0) {
        return -1;
    }

    return 0;
}

static int dg_resolve_usb_path(const char *ifname,
                               const char *devpath_hint,
                               char *out_usb_path,
                               size_t out_usb_path_size) {
    char path[PATH_MAX];
    char device_link[PATH_MAX];
    int n;

    if (devpath_hint != NULL && devpath_hint[0] != '\0') {
        if (dg_extract_usb_path(devpath_hint, out_usb_path, out_usb_path_size) != 0) {
            dg_log(DG_LOG_DEBUG, "usb path unresolved if=%s devpath=%s", ifname, devpath_hint);
            return -1;
        }
        return 0;
    }

    n = snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=device-path-format", ifname);
        return -1;
    }
    if (dg_readlink_into(path, device_link, sizeof(device_link)) != 0) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=device-readlink err=%s", ifname, strerror(errno));
        return -1;
    }
    if (dg_extract_usb_path(device_link, out_usb_path, out_usb_path_size) != 0) {
        dg_log(DG_LOG_DEBUG, "usb path unresolved if=%s device_link=%s", ifname, device_link);
        return -1;
    }

    return 0;
}

static int dg_resolve_ifindex(const char *ifname, int ifindex_hint, unsigned int *out_ifindex) {
    unsigned int ifindex;

    if (ifindex_hint > 0) {
        *out_ifindex = (unsigned int)ifindex_hint;
        return 0;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0U) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=if_nametoindex err=%s", ifname, strerror(errno));
        return -1;
    }

    *out_ifindex = ifindex;
    return 0;
}

static int dg_fill_interface_entry(const char *ifname,
                                   int ifindex_hint,
                                   const char *devpath_hint,
                                   const char *driver_hint,
                                   DgInterfaceEntry *entry) {
    char driver_name[32];
    char usb_path[DG_MAX_PATH_LEN];
    unsigned int ifindex;

    if (ifname == NULL || ifname[0] == '\0') {
        dg_log(DG_LOG_DEBUG, "fill iface skipped: invalid args");
        return -1;
    }

    if (dg_resolve_driver_name(ifname, driver_hint, driver_name, sizeof(driver_name)) != 0) {
        return -1;
    }
    if (dg_resolve_usb_path(ifname, devpath_hint, usb_path, sizeof(usb_path)) != 0) {
        return -1;
    }
    if (dg_resolve_ifindex(ifname, ifindex_hint, &ifindex) != 0) {
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->ifindex = (int)ifindex;
    entry->sock_fd = -1;
    if (snprintf(entry->ifname, sizeof(entry->ifname), "%s", ifname) < 0) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=ifname-copy", ifname);
        return -1;
    }
    if (snprintf(entry->usb_path, sizeof(entry->usb_path), "%s", usb_path) < 0) {
        dg_log(DG_LOG_DEBUG, "fill iface skipped if=%s reason=usb-path-copy", ifname);
        return -1;
    }

    dg_log(DG_LOG_DEBUG, "resolved iface if=%s idx=%d usb_path=%s", entry->ifname, entry->ifindex,
           entry->usb_path);
    return 0;
}

int dg_interface_init(void) {
    DIR *dir;
    struct dirent *de;
    DgInterfaceEntry next[DG_MAX_INTERFACES];
    size_t next_count = 0;
    size_t i;

    dir = opendir("/sys/class/net");
    if (dir == NULL) {
        dg_log(DG_LOG_WARN, "interface init scan failed: %s", strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (next_count >= DG_MAX_INTERFACES) {
            break;
        }
        if (dg_fill_interface_entry(de->d_name, 0, NULL, NULL, &next[next_count]) == 0) {
            (void)dg_try_bind_entry(&next[next_count]);
            next_count++;
        }
    }
    closedir(dir);

    for (i = 0; i < g_entry_count; i++) {
        dg_close_entry_socket(&g_entries[i]);
    }

    memcpy(g_entries, next, sizeof(next));
    g_entry_count = next_count;

    return 0;
}

int dg_interface_upsert(const DgHotplugEvent *evt) {
    DgInterfaceEntry entry;
    int by_index;

    if (dg_fill_interface_entry(evt->ifname, evt->ifindex, evt->devpath, evt->id_net_driver, &entry) != 0) {
        return -1;
    }

    by_index = dg_find_entry_index_by_ifindex(entry.ifindex);
    if (by_index >= 0) {
        entry.sock_fd = g_entries[(size_t)by_index].sock_fd;
        g_entries[(size_t)by_index] = entry;
        if (g_entries[(size_t)by_index].sock_fd < 0) {
            (void)dg_try_bind_entry(&g_entries[(size_t)by_index]);
        }
        return 0;
    }

    if (g_entry_count >= DG_MAX_INTERFACES) {
        return -1;
    }
    (void)dg_try_bind_entry(&entry);
    g_entries[g_entry_count++] = entry;
    return 0;
}

int dg_interface_remove(const DgHotplugEvent *evt) {
    int idx;

    if (evt->ifindex > 0) {
        idx = dg_find_entry_index_by_ifindex(evt->ifindex);
        if (idx >= 0) {
            dg_remove_entry_at((size_t)idx);
            return 0;
        }
    }

    return -1;
}

int dg_interface_is_up(const DgInterfaceEntry *entry) {
    struct ifreq ifr;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return 0;
    }

    memset(&ifr, 0, sizeof(ifr));
    {
        size_t name_len = strnlen(entry->ifname, IFNAMSIZ - 1U);
        memcpy(ifr.ifr_name, entry->ifname, name_len);
        ifr.ifr_name[name_len] = '\0';
    }

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        close(sock);
        return 0;
    }
    close(sock);

    return (ifr.ifr_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) ? 1 : 0;
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
