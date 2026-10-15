# Troubleshooting

## Health checks

```bash
systemctl status dhcp-gadget.service
journalctl -u dhcp-gadget.service -f
ip -br addr show gadget0
bridge link show master gadget0
busctl --system get-property org.dhcp_gadget1 /org/dhcp_gadget1 org.dhcp_gadget1.DHCPServer Leases
```

For detailed logs, set `LOG_LEVEL=debug` in `/etc/default/dhcp-gadget` and restart
the service.

## Common problems

| Symptom | Cause and fix |
| --- | --- |
| Service fails to start; log shows `config parse error at line N` | Fix that line of the config file. |
| `bridge setup failed name=X: interface exists and is not a bridge` | Another interface already uses that name. Set `bridge=` to a free name. |
| `bridge setup failed ... Operation not permitted` | Missing `CAP_NET_ADMIN`. Use the installed service, or run manually as root. |
| `D-Bus initialization failed` when run manually | Install the D-Bus policy first ([installation step 3](installation.md#manual-installation)), and run as root. |
| Gadget plugged in, nothing logged | Its host driver is not cdc_ether/cdc_eem; check with `udevadm info /sys/class/net/<interface> \| grep ID_NET_DRIVER`. With debug logs, look for `reason=unsupported-driver`. |
| `bridge port add failed if=X` | Something else owns the port, such as another bridge or a bond; check `ip -d link show X`. |
| Gadget gets a pool address instead of its static one | The configured USB path does not match; recheck it with the [lookup command](configuration.md#finding-a-gadgets-usb-path). |
| Bridge addresses change, or extra addresses appear | Another network manager still configures the bridge; see [Other network managers](host-bridge.md#other-network-managers). |

Leases are kept in memory, so the `Leases` list is empty after a restart until
gadgets renew.
