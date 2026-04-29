# DHCP Gadget Server

DHCP Gadget is a Linux-focused C daemon for USB gadget networking. It assigns static DHCP leases by USB path and falls back to a dynamic pool for unknown devices.

## Features

- Discover interfaces bound to Linux cdc_ether and cdc_eem drivers
- React to hotplug add, move, and remove events
- Resolve USB interface path values such as 1-2:1.0
- Allocate static DHCP address by USB path
- Fallback to dynamic lease pool
- Structured log output with configurable level

## Build

```bash
make
```

This builds dhcp-gadgetd in the repository root.

## Run

```bash
sudo ./dhcp-gadgetd --config config.example.conf --log-level debug
```

Supported log levels are debug, info, warn, and error.

## Run as systemd service

Install service files and start automatically:

```bash
sudo ./install-systemd.sh
```

This installs:

- daemon binary in /opt/dhcp-gadget/dhcp-gadgetd
- default config in /etc/dhcp-gadget/config.conf (only if missing)
- service env file in /etc/default/dhcp-gadget from dhcp-gadget.env (only if missing)
- unit file in /etc/systemd/system/dhcp-gadget.service

Manage service:

```bash
sudo systemctl status dhcp-gadget.service
sudo systemctl restart dhcp-gadget.service
sudo journalctl -u dhcp-gadget.service -f
```

The service is capability-limited for raw DHCP handling:

- CAP_NET_RAW
- CAP_NET_BIND_SERVICE

If you change [dhcp-gadget.service](dhcp-gadget.service), reload systemd and restart:

```bash
sudo systemctl daemon-reload
sudo systemctl restart dhcp-gadget.service
```

Uninstall service but keep config:

```bash
sudo ./uninstall-systemd.sh
```

Uninstall and purge config and env files:

```bash
sudo ./uninstall-systemd.sh --purge
```

## Configuration format

```ini
# Required keys
subnet=192.168.8.0/24
pool_start=192.168.8.100
pool_end=192.168.8.200

# Optional keys
server_ip=192.168.8.1
router=192.168.8.1
lease_time=86400
dns=1.1.1.1,8.8.8.8

# Repeatable key
static_lease=1-2:1.0=192.168.8.10
static_lease=1-3:1.0=192.168.8.11
```

Use key=value lines, one per setting. Empty lines and lines starting with #, ;, or // are ignored.
router is optional. If omitted, the DHCP router option is not sent.
dns is optional. If omitted, the DHCP DNS option is not sent.

## Notes

- Linux only. Uses AF_PACKET, sysfs, and netlink hotplug events.
- Run with root privileges or with equivalent capabilities.
- USB path matching keys are derived from sysfs interface device paths.
