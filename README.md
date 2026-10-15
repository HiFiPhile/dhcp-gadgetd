# DHCP Gadget Server

`dhcp-gadgetd` gives IPv4 addresses to USB gadgets plugged into a Linux host, so the
host can reach them. Every gadget's network interface joins one host bridge that
carries the server address. A gadget gets the fixed address configured for the USB
port it is plugged into, or an address from a dynamic pool.

```text
gadget A ── USB port 1-3.3:1.0   ── enp0s20f0u3u3   ─┐
gadget B ── USB port 1-3.4.2:1.0 ── enp0s20f0u3u4u2 ─┼── gadget0 bridge, 192.168.8.1/24 (host)
gadget C ── USB port 1-2:1.0     ── enp0s20f0u2     ─┘

static_lease=1-3.3:1.0=192.168.8.20   → gadget A always gets 192.168.8.20
no static lease for 1-2:1.0           → gadget C gets an address from the pool
```

By default, gadgets the host drives with `cdc_ether` (CDC ECM) or `cdc_eem` (CDC EEM)
are served. Set [`drivers`](docs/configuration.md#gadget-drivers) to serve others, such
as `cdc_ncm` (NCM) or `rndis_host` (RNDIS).

Active leases and the configuration are published on the system D-Bus for other
programs to read.

## Quick start

1. Build. You need a C compiler, `make`, `pkg-config`, and the libsystemd headers
   (`libsystemd-dev` on Debian/Ubuntu, `systemd-devel` on Fedora, `systemd-libs` on Arch).

   ```bash
   make
   ```

2. If the host already bridges gadgets with systemd-networkd or NetworkManager,
   remove that setup first; see
   [Moving from a systemd-networkd bridge](docs/host-bridge.md#moving-from-a-systemd-networkd-bridge).

3. Install and start the service. To do it by hand instead, follow
   [Manual installation](docs/installation.md#manual-installation).

   ```bash
   sudo ./install-systemd.sh
   ```

4. Plug in a gadget and check that it got an address:

   ```bash
   ip -br addr show gadget0                 # bridge UP with 192.168.8.1/24
   bridge link show master gadget0          # one line per gadget port
   journalctl -u dhcp-gadget.service -n 20  # "bridge port added", "sent ACK ... ip=..."
   ```

5. To give gadgets fixed addresses,
   [find their USB paths](docs/configuration.md#finding-a-gadgets-usb-path), add
   `static_lease` lines to `/etc/dhcp-gadget/config.conf`, and restart:

   ```bash
   sudo systemctl restart dhcp-gadget.service
   ```

## Documentation

| Document | Contents |
| --- | --- |
| [Installation](docs/installation.md) | Requirements, installer script, manual installation, service settings, uninstalling |
| [Configuration](docs/configuration.md) | Configuration keys, gadget drivers, finding USB paths, custom DHCP options |
| [Host bridge](docs/host-bridge.md) | What the daemon does to the bridge, other network managers, moving from systemd-networkd |
| [Troubleshooting](docs/troubleshooting.md) | Health checks and common problems |
| [D-Bus API](docs/dbus-api.md) | Properties, interface XML, lease change signals, Python listener |
| [Development](docs/development.md) | Building, tests, running without root, how it works |
