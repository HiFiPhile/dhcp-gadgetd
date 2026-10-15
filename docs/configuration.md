# Configuration

The service reads `/etc/dhcp-gadget/config.conf`, installed from
[config.example.conf](../config.example.conf) if it does not exist. Changes take
effect after a restart:

```bash
sudo systemctl restart dhcp-gadget.service
```

Write one `key=value` per line. Empty lines and lines starting with `#`, `;`, or `//`
are ignored. Unknown keys and invalid values stop the daemon at startup; the log names
the line (`config parse error at line N`).

## Keys

| Key | Required | Default | Meaning |
| --- | --- | --- | --- |
| `subnet` | yes | | Network served, in CIDR form, e.g. `192.168.8.0/24` |
| `pool_start` | yes | | First dynamic pool address |
| `pool_end` | yes | | Last dynamic pool address (inclusive) |
| `server_ip` | no | first host of `subnet` | Host address on the bridge; also the DHCP server identifier |
| `router` | no | not sent | Router (option 3) sent to gadgets |
| `dns` | no | not sent | Up to 4 DNS servers, comma-separated |
| `lease_time` | no | `86400` | Lease duration in seconds |
| `bridge` | no | `gadget0` | Bridge the daemon manages; see [Host bridge](host-bridge.md) |
| `drivers` | no | `cdc_ether,cdc_eem` | Host network drivers whose interfaces are gadget ports; see [Gadget drivers](#gadget-drivers) |
| `static_lease` | no, repeatable | | `<usb path>=<address>`, up to 128 entries |
| `custom_option` | no, repeatable | | `<code>=<hex bytes>`; see [Custom DHCP options](#custom-dhcp-options) |

Address rules:

- `server_ip`, `router`, the pool, and every static lease address must be inside `subnet`.
- Static lease addresses must be outside the pool and differ from `server_ip` and `router`.
  Each USB path and each address may appear only once.
- Keep `server_ip` and `router` outside the pool, so the pool never hands them out.

`bridge` must be a valid interface name: at most 15 characters, without `/`, `:`, or
whitespace.

Example:

```ini
subnet=192.168.8.0/24
server_ip=192.168.8.1
pool_start=192.168.8.100
pool_end=192.168.8.200
lease_time=900
dns=1.1.1.1,8.8.8.8

# Gadgets on a hub plugged into root port 3 of bus 1
static_lease=1-3.3:1.0=192.168.8.20
static_lease=1-3.4.2:1.0=192.168.8.24
```

## Gadget drivers

`drivers` lists the host network drivers whose interfaces the daemon treats as
gadget ports: it serves DHCP on them and adds them to the bridge. The default is
`cdc_ether,cdc_eem`. To also serve NCM and RNDIS gadgets:

```ini
drivers=cdc_ether,cdc_eem,cdc_ncm,rndis_host
```

To find the driver of a plugged-in gadget, use the
[listing below](#finding-a-gadgets-usb-path).

- List up to 8 drivers, comma-separated. Names may contain letters, digits, `_`,
  and `-`, up to 31 characters.
- Only interfaces on a USB device are used, since static leases need a USB path.
  Other interfaces with a listed driver are skipped.
- Every USB interface with a listed driver joins the bridge, including the host's
  own USB network adapters. Some USB Ethernet adapters and LTE modems use
  `cdc_ether`, for example.

After changing `drivers`, re-run `sudo ./install-systemd.sh`. It keeps your
configuration, regenerates the udev rule that keeps other network managers off
these ports, and restarts the service. For a manual installation, repeat
[step 5](installation.md#manual-installation) and restart the service. Ports whose
driver you removed stay in the bridge until they are unplugged.

## Finding a gadget's USB path

Plug the gadget in and list network interfaces with their driver and device:

```bash
for dev in /sys/class/net/*/device; do
  echo "$(basename "$(dirname "$dev")") $(basename "$(readlink "$dev/driver")") $(basename "$(readlink -f "$dev")")"
done
```

```text
enp1s0 r8169 0000:01:00.0
enp0s20f0u3u3 cdc_eem 1-3.3:1.0
enp0s20f0u3u4u2 cdc_eem 1-3.4.2:1.0
```

Gadgets are the rows with a USB path such as `1-3.3:1.0` in the last column, while
PCI devices show an address like `0000:01:00.0`. The middle column must be listed in
[`drivers`](#gadget-drivers). The last column is the value for `static_lease`.
`1-3.4.2:1.0` reads as bus 1, root port 3, hub port 4, hub port 2, then
configuration 1, interface 0.

The path belongs to the physical port, not the gadget. Any gadget plugged into that
port gets its address, and moving a hub to another root port changes the paths of
everything behind it.

## Custom DHCP options

`custom_option` adds an option to every OFFER and ACK. Its value is a decimal option
code, `=`, and one or more colon-separated hexadecimal bytes:

```ini
custom_option=42=c0:a8:08:02
custom_option=224=01:02:ff
```

Codes 0 and 255, codes the server manages (1, 3, 6, 51, 53, and 54), duplicate codes,
and payloads larger than 255 bytes are rejected. At most 32 custom options are
allowed, with a combined encoded size of 262 bytes, which keeps replies within the
DHCP minimum packet size of 576 bytes.
