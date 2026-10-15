#!/usr/bin/python3
"""Print DHCP Gadget snapshots and lease changes as JSON on the system bus.

Debian/Ubuntu: sudo apt install python3-dbus python3-gi
Run: /usr/bin/python3 examples/listen_leases.py
Uses dbus-python's GLib integration:
https://dbus.freedesktop.org/doc/dbus-python/tutorial.html
"""

import ipaddress
import json
import sys
import time

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BUS_NAME = "org.dhcp_gadget1"
OBJECT_PATH = "/org/dhcp_gadget1"
INTERFACE = "org.dhcp_gadget1.DHCPServer"
PROPERTIES = "org.freedesktop.DBus.Properties"
ADDRESS_PROPERTIES = {
    "ServerAddress", "SubnetAddress", "SubnetMask", "Router", "PoolStart", "PoolEnd"
}


def address(value):
    return str(ipaddress.IPv4Address(bytes(value))) if value else None


def decode(name, value):
    if name in ADDRESS_PROPERTIES:
        return address(value)
    if name == "DNS":
        return [address(item) for item in value]
    if name == "StaticLeases":
        return [{"usb_path": str(path), "address": address(ip)} for path, ip in value]
    if name in {"Options", "CustomOptions"}:
        return [{"code": int(code), "data": bytes(data).hex(":")} for code, data in value]
    if name == "Leases":
        now_usec = time.monotonic_ns() // 1000
        return [
            {
                "family": int(family),
                "client_id": bytes(client_id).hex(":"),
                "address": address(ip),
                "gateway": address(gateway),
                "mac": bytes(mac).hex(":"),
                # The daemon uses CLOCK_MONOTONIC, not Unix time.
                "expires_monotonic_usec": int(expiration),
                "remaining_seconds": max(0, (int(expiration) - now_usec) / 1_000_000),
            }
            for family, client_id, ip, gateway, mac, expiration in value
        ]
    return value  # Remaining properties are JSON-compatible strings, integers, or string arrays.


def emit(event, properties):
    print(json.dumps({"event": event, "properties": {
        str(name): decode(str(name), value) for name, value in properties.items()
    }}, sort_keys=True), flush=True)


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    loop = GLib.MainLoop()
    owner = ""
    generation = 0

    def snapshot():
        current_generation = generation
        proxy = bus.get_object(owner, OBJECT_PATH, introspect=False)

        def received(properties):
            if current_generation == generation:
                emit("snapshot", properties)

        def failed(error):
            if current_generation == generation:
                print(f"Cannot read DHCP Gadget properties: {error}", file=sys.stderr)

        proxy.GetAll(INTERFACE, dbus_interface=PROPERTIES, signature="s",
                     reply_handler=received, error_handler=failed)

    def changed(interface, properties, invalidated, sender=None):
        if str(interface) != INTERFACE or sender != owner:
            return
        if properties:
            emit("changed", properties)
        if invalidated:
            snapshot()

    def owner_changed(new_owner):
        nonlocal owner, generation
        owner = str(new_owner)
        generation += 1
        if owner:
            snapshot()
        else:
            emit("unavailable", {})

    # Subscribe before requesting the initial snapshot. Track restarts too.
    match = bus.add_signal_receiver(
        changed, signal_name="PropertiesChanged", dbus_interface=PROPERTIES,
        path=OBJECT_PATH, arg0=INTERFACE, sender_keyword="sender",
    )
    watch = bus.watch_name_owner(BUS_NAME, owner_changed)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        watch.cancel()
        match.remove()


if __name__ == "__main__":
    try:
        main()
    except dbus.DBusException as error:
        print(f"D-Bus connection failed: {error}", file=sys.stderr)
        sys.exit(1)
