# Development

## Building and testing

```bash
make
make test
```

`make` builds `dhcp-gadgetd` in the repository root. `make test` also needs
`dbus-run-session`.

The bridge test runs in a throwaway network namespace through `unshare -rn`, which
needs unprivileged user namespaces. Where they are blocked (for example by Ubuntu
24.04's AppArmor defaults), run `make test NETNS="sudo unshare -n"`.

## Running the daemon without root

The daemon can run fully, bridge and D-Bus included, inside a throwaway network
namespace with a private bus standing in for the system bus. Nothing on the host
changes, but no gadgets are visible either.

```bash
unshare -rn dbus-run-session -- sh -c '
  DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS ./dhcp-gadgetd --config config.example.conf &
  sleep 1
  busctl --user get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Bridge
  kill $!'
```

## Updating the interface XML

[org.dhcp_gadget1.DHCPServer.xml](org.dhcp_gadget1.DHCPServer.xml) must match the
property table in `src/bus.c`, and `tests/bus_test.c` compares the two, ignoring
comments and whitespace. After changing properties, print the daemon's current
interface:

```bash
unshare -rn dbus-run-session -- sh -c '
  DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS ./dhcp-gadgetd --config config.example.conf 2>/dev/null &
  sleep 1
  busctl --user introspect --xml-interface org.dhcp_gadget1 /org/dhcp_gadget1
  kill $!'
```

Copy the `org.dhcp_gadget1.DHCPServer` interface element into the XML file, keep
or write a comment for each property, and update the property table in
[dbus-api.md](dbus-api.md).

## How it works

- Linux only. Interfaces are found through sysfs and followed through udev netlink
  hotplug events (add, move, remove).
- The USB path of each interface comes from its sysfs device path.
- DHCP packets are read and sent on each port with AF_PACKET sockets, not on the
  bridge, so the daemon knows which USB port a request came from. The sockets bind
  with `ETH_P_ALL` because the bridge consumes frames before protocol-specific
  packet sockets see them.
- The bridge is configured over rtnetlink (`src/bridge.c`); see [Host bridge](host-bridge.md).
- Renewals sent by unicast to `server_ip` also reach the host's own UDP stack, which
  answers with ICMP port unreachable. The daemon still replies, and common DHCP
  clients ignore the ICMP message.
