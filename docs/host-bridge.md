# Host bridge

The daemon owns one bridge (`gadget0` unless [`bridge`](configuration.md#keys) is set)
and adds every USB interface using one of the configured
[`drivers`](configuration.md#gadget-drivers) to it, so the host reaches all gadgets
through a single interface. At startup it:

- creates the bridge, or adopts an existing interface of that name if it is a bridge
- sets a stable MAC derived from `/etc/machine-id`; otherwise the bridge would copy the
  lowest port MAC, which changes as gadgets come and go
- brings the bridge up and makes `server_ip` with the subnet prefix its only IPv4
  address, removing any other IPv4 address on it
- adds every gadget port to the bridge and brings it up; hotplugged ports follow

Stopping the daemon keeps the bridge, so gadgets stay reachable across restarts;
uninstalling deletes it. If the bridge cannot be created or configured, startup
fails. A port that cannot join the bridge is logged and still gets DHCP replies,
but the host cannot reach it.

## Other network managers

Only the daemon may configure the bridge and its ports. The installed udev rule,
generated from [90-dhcp-gadget.rules.in](../90-dhcp-gadget.rules.in) with the
configured drivers, marks gadget ports `NM_UNMANAGED` for NetworkManager and sets
`ID_NET_MANAGED_BY=org.dhcp_gadget1` for systemd-networkd, so neither touches them.
The rule only follows `drivers` when it is regenerated; see
[Gadget drivers](configuration.md#gadget-drivers).

You still have to remove any systemd-networkd `.netdev`/`.network` file or
NetworkManager profile for the bridge itself. Do not run another DHCP server on the
bridge (networkd `DHCPServer=`, dnsmasq).

Host USB network adapters that use a configured driver are treated as gadget ports
too.

## Moving from a systemd-networkd bridge

1. Find the files that create the bridge, configure it, or add ports to it with
   `Bridge=`. Check each one listed, then remove it:

   ```bash
   grep -l gadget0 /etc/systemd/network/*
   sudo rm /etc/systemd/network/<file>
   sudo networkctl reload
   ```

2. Install or restart the daemon. It adopts the existing bridge and removes every
   IPv4 address except `server_ip`, including link-local `169.254.x.x` addresses from
   `LinkLocalAddressing=`. For a clean start, delete the old bridge first with
   `sudo ip link delete gadget0`; gadgets are unreachable until the daemon recreates it.

NetworkManager `unmanaged-devices=driver:cdc_ether` or `driver:cdc_eem` settings can
stay; the udev rule does the same thing.
