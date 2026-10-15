# D-Bus API

The system bus name is `org.dhcp_gadget1`, the object path is
`/org/dhcp_gadget1`, and the interface is `org.dhcp_gadget1.DHCPServer`.
This single object covers the shared address pool across all gadget interfaces.
All properties are read-only; standard Introspectable, Peer, and Properties
interfaces are provided by sd-bus.

The installed policy allows local clients to read the interface and permits only
root and `dhcp-gadget` to own the service name. The systemd unit waits for that
name before declaring the daemon ready and restarts it if the bus disconnects.

## Properties

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
| Bridge | `s` | Name of the bridge carrying the gadget ports |
| ConfigFile | `s` | Configuration file path supplied to the daemon |

All configuration properties reflect the loaded configuration, including defaults,
and are constant for the daemon lifetime. Restart after editing the file.
IPv4 configuration values contain four bytes in network order (except an absent
`Router`, which has zero bytes). `StaticLeases` lists reservations independently
of the currently active `Leases`.

### Leases

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

### Options

`Options` includes server identifier (54), subnet mask (1), lease time (51),
optional router (3), optional DNS (6), and all configured custom options. Values
are exactly the payload bytes used in OFFER/ACK packets, excluding their code and
length bytes. Message type (53), padding, and END are omitted. `Options`,
`PoolSize`, and `PoolOffset` are constant for the daemon lifetime; restart after
editing configuration. `Options` is a gadget extension to the systemd layout.

## Interface XML

[org.dhcp_gadget1.DHCPServer.xml](org.dhcp_gadget1.DHCPServer.xml) is the
introspection data for the interface, with a comment on each property. Use it with
code generators such as `gdbus-codegen`, `qdbusxml2cpp`, or `sdbus-c++-xml2cpp`.
`make test` fails if it no longer matches what the daemon publishes; see
[Updating the interface XML](development.md#updating-the-interface-xml).

To read the same data from a running daemon:

```bash
busctl --system introspect --xml-interface org.dhcp_gadget1 /org/dhcp_gadget1
```

## busctl examples

```bash
busctl --system introspect org.dhcp_gadget1 /org/dhcp_gadget1
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Leases
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Options
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer StaticLeases
busctl --system call org.dhcp_gadget1 /org/dhcp_gadget1 org.freedesktop.DBus.Properties GetAll s org.dhcp_gadget1.DHCPServer
busctl --system monitor org.dhcp_gadget1
```

## Python listener example

[examples/listen_leases.py](../examples/listen_leases.py) prints a JSON snapshot of
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
