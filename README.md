# DHCP Gadget Server

DHCP Gadget is a Linux-focused C daemon for USB gadget networking. It assigns static DHCP leases by USB path and falls back to a dynamic pool for unknown devices.

## Features

- Discover interfaces bound to Linux cdc_ether and cdc_eem drivers
- React to hotplug add, move, and remove events
- Resolve USB interface path values such as 1-2:1.0
- Allocate static DHCP address by USB path
- Fallback to dynamic lease pool
- Publish active leases and DHCP options over the system D-Bus
- Structured log output with configurable level

## Build

Requires a C compiler, `pkg-config`, and the libsystemd development package
(`libsystemd-dev` on Debian/Ubuntu). Tests also require `dbus-run-session`.

```bash
make
make test
```

This builds dhcp-gadgetd in the repository root.

## Run

```bash
sudo ./dhcp-gadgetd --config config.example.conf --log-level debug
```

Supported log levels are debug, info, warn, and error. Install the D-Bus policy
with the service installer before running manually. The daemon requires access
to the system bus and fails startup if its bus name cannot be acquired.

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
- D-Bus policy in /etc/dbus-1/system.d/org.dhcp_gadget1.conf
- dedicated system user/group `dhcp-gadget` via `systemd-sysusers`

Installation stops an existing service before creating its permanent account
and restarts it after installation, including when upgrading from `DynamicUser`.
Uninstallation
removes the D-Bus policy but retains the system account.

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

## D-Bus API

The system bus name is `org.dhcp_gadget1`, the object path is
`/org/dhcp_gadget1`, and the interface is `org.dhcp_gadget1.DHCPServer`.
This single object covers the shared address pool across all gadget interfaces.
All properties are read-only; standard Introspectable, Peer, and Properties
interfaces are provided by sd-bus.

| Property | D-Bus type | Meaning |
| --- | --- | --- |
| Leases | `a(uayayayayt)` | Active, successfully acknowledged static and dynamic leases |
| Options | `a(yay)` | DHCP option code and raw payload bytes, in wire order |
| PoolSize | `u` | Inclusive configured dynamic pool size |
| PoolOffset | `u` | Pool start offset from the subnet network address |
| ServerAddress | `ay` | Server IPv4 address |
| SubnetAddress | `ay` | Subnet network address |
| SubnetMask | `ay` | IPv4 subnet mask |
| SubnetPrefixLength | `y` | Subnet prefix length in bits |
| Router | `ay` | Router IPv4 address; empty when omitted |
| DNS | `aay` | DNS server IPv4 addresses in configured order; empty when omitted |
| PoolStart | `ay` | First dynamic pool IPv4 address |
| PoolEnd | `ay` | Last dynamic pool IPv4 address |
| LeaseTime | `u` | Configured lease duration in seconds |
| StaticLeases | `a(say)` | Configured USB path and IPv4 address pairs, including unused reservations |
| CustomOptions | `a(yay)` | Only explicitly configured custom option codes and raw payloads |
| ConfigFile | `s` | Configuration file path supplied to the daemon |

All configuration properties reflect the loaded configuration, including defaults,
and are constant for the daemon lifetime. Restart after editing the file.
IPv4 configuration values contain four bytes in network order (except an absent
`Router`, which has zero bytes). `StaticLeases` lists reservations independently
of the currently active `Leases`.

`Leases` uses the tuple layout of
[systemd's DHCPServer interface](https://github.com/systemd/systemd/blob/main/src/network/networkd-dhcp-server-bus.c):
address family (`AF_INET`, 2), client identifier, IPv4 address, gateway, hardware
address, and expiration in microseconds. In this daemon the expiration is an
absolute `CLOCK_MONOTONIC` timestamp, not Unix time. IPv4 addresses are four bytes
in network order; the gateway is zero when no router is configured. Client IDs
preserve DHCP option 61; when absent, the ID is Ethernet type byte `1` followed
by the six-byte MAC. Allocation continues to use the MAC/USB path as before.

Offers and unused static reservations are excluded. Successful ACKs and renewals,
releases, and expiry emit `org.freedesktop.DBus.Properties.PropertiesChanged`
for `Leases`; multiple changes in one server tick may be combined. Leases are
kept in memory and disappear on daemon restart. Interface removal alone does not
release a lease; it remains active until release or expiry.

`Options` includes server identifier (54), subnet mask (1), lease time (51),
optional router (3), optional DNS (6), and all configured custom options. Values
are exactly the payload bytes used in OFFER/ACK packets, excluding their code and
length bytes. Message type (53), padding, and END are omitted. `Options`,
`PoolSize`, and `PoolOffset` are constant for the daemon lifetime; restart after
editing configuration. `Options` is a gadget extension to the systemd layout.

```bash
busctl --system introspect org.dhcp_gadget1 /org/dhcp_gadget1
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Leases
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Options
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer StaticLeases
busctl --system call org.dhcp_gadget1 /org/dhcp_gadget1 org.freedesktop.DBus.Properties GetAll s org.dhcp_gadget1.DHCPServer
busctl --system monitor org.dhcp_gadget1
```

The installed policy allows local clients to read the interface and permits only
root and `dhcp-gadget` to own the service name. The systemd unit waits for that
name before declaring the daemon ready and restarts it if the bus disconnects.

### Python listener example

[examples/listen_leases.py](examples/listen_leases.py) prints a JSON snapshot of
all configuration and active leases, then listens for `PropertiesChanged`.
It decodes IPv4 addresses, MACs, option payloads, and remaining lease duration.
Each changed `Leases` value is the complete current lease list, so an empty list
means no active leases. Remaining duration is calculated when the event arrives.
The listener waits if the daemon is absent and reads a new snapshot on restart.

On Debian/Ubuntu:

```bash
sudo apt install python3-dbus python3-gi
/usr/bin/python3 examples/listen_leases.py
```

Run the listener as your normal user; press Ctrl+C to stop. Use the system Python
so it can import the distribution's D-Bus and GLib packages. Output is one JSON
object per line with an `event` of `snapshot`, `changed`, or `unavailable`.

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

# Repeatable custom DHCP option: decimal code and colon-separated hex bytes
custom_option=42=c0:a8:08:02
custom_option=224=01:02:ff
```

Use key=value lines, one per setting. Empty lines and lines starting with #, ;, or // are ignored.
router is optional. If omitted, the DHCP router option is not sent.
dns is optional. If omitted, the DHCP DNS option is not sent.
custom_option is optional and repeatable. Its value is a decimal DHCP option code,
followed by `=`, followed by one or more colon-separated hexadecimal bytes. Custom
options are included in DHCP OFFER and ACK replies. Codes 0 and 255, codes managed
by the server (1, 3, 6, 51, 53, and 54), duplicate codes, and payloads larger than
255 bytes are rejected. At most 32 custom options may be configured, with a combined
encoded size of 262 bytes; this keeps replies within the DHCP 576-byte minimum packet
size.

## Notes

- Linux only. Uses AF_PACKET, sysfs, and netlink hotplug events.
- Run with root privileges or with equivalent capabilities.
- USB path matching keys are derived from sysfs interface device paths.
