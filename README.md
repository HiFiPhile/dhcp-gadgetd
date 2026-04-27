# DHCP Gadget Server

Simple Python DHCP server for USB gadget bridge setups.

## Features

- Select bridge interface (for example `gadget0`)
- Discover attached lower interfaces from `/sys/class/net/<bridge>/lower_*`
- Resolve USB device path from each lower interface (for example `1-2:1.0`)
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
sudo python3 dhcp_server.py --bridge gadget0 --config config.example.json
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

## Notes

- This implementation is Linux-focused and uses sysfs paths.
- Run with root privileges (or equivalent capabilities) because packet sniffing/sending is raw.
- USB path format comes from sysfs resolution for each attached lower interface.
