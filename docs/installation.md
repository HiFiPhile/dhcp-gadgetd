# Installation

## Requirements

- Linux with systemd, udev, and the D-Bus system bus.
- To build: a C compiler, `make`, `pkg-config`, and the libsystemd headers
  (`libsystemd-dev` on Debian/Ubuntu, `systemd-devel` on Fedora, `systemd-libs` on Arch).
- iproute2 (`ip`), used to delete the bridge when uninstalling.

Build in the repository root; this produces `./dhcp-gadgetd`:

```bash
make
```

## Install with the script

```bash
sudo ./install-systemd.sh
```

The script stops a running service, installs the files below, then enables and
restarts the service.

| Repository file | Installed as | Purpose |
| --- | --- | --- |
| `dhcp-gadgetd` | `/opt/dhcp-gadget/dhcp-gadgetd` | The daemon |
| `config.example.conf` | `/etc/dhcp-gadget/config.conf` | [Configuration](configuration.md); only if missing |
| `dhcp-gadget.env` | `/etc/default/dhcp-gadget` | [Service environment](#service-settings); only if missing |
| `dhcp-gadget.service` | `/etc/systemd/system/dhcp-gadget.service` | systemd unit |
| `org.dhcp_gadget1.conf` | `/etc/dbus-1/system.d/org.dhcp_gadget1.conf` | D-Bus policy: only root and `dhcp-gadget` may own the bus name; anyone may read it |
| `90-dhcp-gadget.rules.in` | `/etc/udev/rules.d/90-dhcp-gadget.rules` | Generated with the configured [`drivers`](configuration.md#gadget-drivers); keeps NetworkManager and systemd-networkd off gadget ports, see [Other network managers](host-bridge.md#other-network-managers) |
| `dhcp-gadget.sysusers` | not copied; applied once | Creates the system user and group `dhcp-gadget` |

Re-running the script upgrades the binary, unit, and D-Bus policy, regenerates the
udev rule from the current `drivers`, and keeps your config and environment file. It also handles upgrading from an older
install that used `DynamicUser`.

## Manual installation

These steps do what the script does. Run them from the repository root after `make`.

1. When upgrading, stop the service first. This is required when the old install
   used `DynamicUser`: while it runs, its account hides the missing permanent one,
   and step 2 would skip creating it.

   ```bash
   sudo systemctl stop dhcp-gadget.service
   ```

2. Create the system user and group. The D-Bus policy refers to this account, so
   it must exist first. Keep the `./`: without a slash, `systemd-sysusers` looks
   for the name in its own config directories instead of the current one.

   ```bash
   sudo systemd-sysusers ./dhcp-gadget.sysusers
   getent passwd dhcp-gadget
   ```

3. Install the D-Bus policy and make the bus reload its configuration:

   ```bash
   sudo install -Dm644 org.dhcp_gadget1.conf /etc/dbus-1/system.d/org.dhcp_gadget1.conf
   sudo busctl call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
   ```

4. Install the daemon, the configuration, and the service environment file. When
   upgrading, skip the last two lines; `install` overwrites existing files.

   ```bash
   sudo install -Dm755 dhcp-gadgetd /opt/dhcp-gadget/dhcp-gadgetd
   sudo install -Dm644 config.example.conf /etc/dhcp-gadget/config.conf
   sudo install -Dm644 dhcp-gadget.env /etc/default/dhcp-gadget
   ```

   Edit `/etc/dhcp-gadget/config.conf` now or later; see [Configuration](configuration.md).
   Set [`drivers`](configuration.md#gadget-drivers) before the next step, which reads it.

5. Generate the udev rule from the configured drivers, then apply it to ports that
   are already plugged in. `--print-drivers` validates the config and prints one
   driver per line. The trigger also covers ports that an earlier rule marked, so a
   removed driver is released.

   ```bash
   drivers=$(/opt/dhcp-gadget/dhcp-gadgetd --config /etc/dhcp-gadget/config.conf --print-drivers)
   sed "s/@DRIVERS@/$(paste -sd'|' <<< "$drivers")/" 90-dhcp-gadget.rules.in |
     sudo tee /etc/udev/rules.d/90-dhcp-gadget.rules > /dev/null
   sudo udevadm control --reload
   sudo udevadm trigger --action=change --subsystem-match=net \
     --property-match=ID_NET_MANAGED_BY=org.dhcp_gadget1 \
     $(sed 's/^/--property-match=ID_NET_DRIVER=/' <<< "$drivers")
   ```

6. Install the unit, then enable and start the service. `reset-failed` clears an
   earlier start-limit failure, which would otherwise block the restart.

   ```bash
   sudo install -Dm644 dhcp-gadget.service /etc/systemd/system/dhcp-gadget.service
   sudo systemctl daemon-reload
   sudo systemctl enable dhcp-gadget.service
   sudo systemctl reset-failed dhcp-gadget.service
   sudo systemctl restart dhcp-gadget.service
   ```

7. Check that it is running and owns the bridge:

   ```bash
   systemctl status dhcp-gadget.service
   ip -br addr show gadget0
   ```

The unit starts `/opt/dhcp-gadget/dhcp-gadgetd`. To install the binary elsewhere,
change `ExecStart` with a [drop-in](#service-settings). To move the config file,
change `CONFIG` in the environment file.

## Service settings

`/etc/default/dhcp-gadget` sets:

| Variable | Default | Meaning |
| --- | --- | --- |
| `CONFIG` | `/etc/dhcp-gadget/config.conf` | Configuration file |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn`, or `error` |

The service runs as `dhcp-gadget` with only these capabilities:

- `CAP_NET_ADMIN` to manage the bridge
- `CAP_NET_RAW` for the DHCP packet sockets
- `CAP_NET_BIND_SERVICE`

The installer overwrites the unit file, so put local changes in a drop-in, which
survives reinstalls:

```bash
sudo systemctl edit dhcp-gadget.service
```

Everyday commands:

```bash
sudo systemctl status dhcp-gadget.service
sudo systemctl restart dhcp-gadget.service
sudo journalctl -u dhcp-gadget.service -f
```

## Uninstall

Remove the service, D-Bus policy, udev rule, and bridge, keeping the config and
environment file:

```bash
sudo ./uninstall-systemd.sh
```

Also delete the config and environment file:

```bash
sudo ./uninstall-systemd.sh --purge
```

The `dhcp-gadget` system account is kept in both cases.

To uninstall by hand:

```bash
sudo systemctl disable --now dhcp-gadget.service
sudo ip link delete dev gadget0        # or the bridge name set in the config
sudo rm /etc/udev/rules.d/90-dhcp-gadget.rules
sudo udevadm control --reload
sudo udevadm trigger --action=change --subsystem-match=net --property-match=ID_NET_MANAGED_BY=org.dhcp_gadget1
sudo rm /etc/dbus-1/system.d/org.dhcp_gadget1.conf
sudo busctl call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
sudo rm /etc/systemd/system/dhcp-gadget.service
sudo systemctl daemon-reload
sudo rm /opt/dhcp-gadget/dhcp-gadgetd
sudo rmdir /opt/dhcp-gadget
# Optional: configuration and environment file
sudo rm -r /etc/dhcp-gadget /etc/default/dhcp-gadget
```

Deleting the bridge disconnects the gadgets. The `udevadm trigger` line hands their
ports back to the host's network managers.

## Running in the foreground

To debug, run the daemon from a shell instead of as the service. Stop the service
first: both would manage the same bridge.

```bash
sudo systemctl stop dhcp-gadget.service
sudo ./dhcp-gadgetd --config /etc/dhcp-gadget/config.conf --log-level debug
```

The daemon must be able to own its system bus name, so the D-Bus policy must be
installed (step 3 above); it fails to start otherwise.
