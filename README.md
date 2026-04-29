# DHCP Gadget Server

Tired of randomly changing DHCP addresses for your USB gadget devices? This simple Python DHCP server allows you to assign static IP addresses based on the USB device path, making it easier to manage and connect to your gadgets.

## Features

- Discover interfaces bound to Linux `cdc_ether` and `cdc_eem` drivers
- React to udev hotplug events and bind packet receivers on all active supported interfaces
- Resolve USB device path from each active interface (for example `1-2:1.0`)
- Allocate static DHCP address by USB device path
- Fallback to dynamic pool for unknown USB device paths

## Files

- `dhcp_server.py`: DHCP server implementation
- `config.example.json`: Example network and static-lease configuration
- `requirements.txt`: Python dependencies

## Install

```bash
pip install -r requirements.txt
```

## Run

```bash
sudo python3 dhcp_server.py --config config.example.json
```

## Run as systemd service

Install service files and start automatically:

```bash
sudo ./install-systemd.sh
```

This installs:

- app code in `/opt/dhcp-gadget`
- default config in `/etc/dhcp-gadget/config.json` (only if missing)
- service env file in `/etc/default/dhcp-gadget`
- unit file in `/etc/systemd/system/dhcp-gadget.service`

Manage service:

```bash
sudo systemctl status dhcp-gadget.service
sudo systemctl restart dhcp-gadget.service
sudo journalctl -u dhcp-gadget.service -f
```

The unit is hardened with a limited capability set:

- `CAP_NET_RAW`
- `CAP_NET_BIND_SERVICE`

If you change [dhcp-gadget.service](dhcp-gadget.service), reinstall or copy the unit again, then run:

```bash
sudo systemctl daemon-reload
sudo systemctl restart dhcp-gadget.service
```

Uninstall service but keep config:

```bash
sudo ./uninstall-systemd.sh
```

Uninstall and purge config/env files too:

```bash
sudo ./uninstall-systemd.sh --purge
```

## Configuration format

```json
{
  "subnet": "192.168.7.0/24",
  "router": "192.168.7.1",
  "server_ip": "192.168.7.1",
  "lease_time": 86400,
  "pool": {"start": "192.168.7.100", "end": "192.168.7.200"},
  "static_leases": {
    "1-2:1.0": "192.168.7.10"
  }
}
```

`router` is optional. If omitted, the DHCP `router` option is not sent.
`dns` is optional. If omitted, the DHCP DNS option is not sent.

Interfaces are auto-discovered from `cdc_ether`/`cdc_eem` driver bindings, so no bridge setting is required.

Replies are sent with raw socket `sendto()` on the selected egress interface.

## Notes

- This implementation is Linux-focused and uses sysfs paths.
- Interface hotplug tracking is udev-based and requires `pyudev`.
- Run with root privileges (or equivalent capabilities) because packet sniffing/sending is raw.
- USB path format comes from sysfs resolution for each attached interface.

## C Rewrite Bootstrap

An initial C daemon scaffold is now present for the rewrite effort.

Build:

```bash
make
```

Run:

```bash
sudo ./dhcp-gadgetd --config config.example.json --log-level debug
```

Current status of the C daemon:

- module boundaries and server loop are in place
- signal handling and logging are in place
- config parser is bootstrap-only (loads defaults and validates file readability)
- DHCP wire parsing/building and hotplug integrations are stubbed for next implementation phases
