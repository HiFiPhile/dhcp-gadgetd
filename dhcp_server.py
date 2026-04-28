#!/usr/bin/env python3
"""Simple DHCP server for USB gadget cdc_ether/cdc_eem setups.

Features:
- Discover active network interfaces bound to cdc_ether/cdc_eem drivers
- Resolve USB device path for each supported interface (example: 1-2:1.0)
- Allocate static IP by USB device path
- Fallback to dynamic pool for unknown device paths
"""

from __future__ import annotations

import argparse
import errno
import ipaddress
import json
import logging
import re
import select
import signal
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

try:
    from scapy.all import (
        BOOTP,
        DHCP,
        Ether,
        IP,
        UDP,
        conf,
        get_if_hwaddr,
    )
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit(
        "scapy is required. Install dependencies with: pip install -r requirements.txt"
    ) from exc

try:
    import pyudev
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit(
        "pyudev is required for udev hotplug handling. Install dependencies with: pip install -r requirements.txt"
    ) from exc


LOG = logging.getLogger("dhcp_gadget")
USB_IFACE_RE = re.compile(r"^\d+-\d+(?:\.\d+)*:\d+\.\d+$")
USB_DEVICE_RE = re.compile(r"^\d+-\d+(?:\.\d+)*$")
ETH_P_ALL = 0x0003
SUPPORTED_USB_NET_DRIVERS = {"cdc_ether", "cdc_eem"}


@dataclass
class Lease:
    ip: str
    expires_at: float


class ConfigError(ValueError):
    """Raised when configuration is invalid."""


class LeaseAllocator:
    def __init__(
        self,
        pool_start: str,
        pool_end: str,
        lease_seconds: int,
        static_by_usb_path: dict[str, str],
    ) -> None:
        self.pool_start = ipaddress.IPv4Address(pool_start)
        self.pool_end = ipaddress.IPv4Address(pool_end)
        if int(self.pool_end) < int(self.pool_start):
            raise ConfigError("pool.end must be greater than or equal to pool.start")

        self.lease_seconds = lease_seconds
        self.static_by_usb_path = static_by_usb_path
        self.static_ips = set(static_by_usb_path.values())

        self.dynamic_by_mac: dict[str, Lease] = {}
        self.dynamic_by_ip: dict[str, str] = {}

    def _cleanup_expired(self) -> None:
        now = time.time()
        expired_macs = [
            mac for mac, lease in self.dynamic_by_mac.items() if lease.expires_at <= now
        ]
        for mac in expired_macs:
            lease = self.dynamic_by_mac.pop(mac)
            self.dynamic_by_ip.pop(lease.ip, None)

    def _allocate_dynamic(self, mac: str) -> Optional[str]:
        self._cleanup_expired()

        existing = self.dynamic_by_mac.get(mac)
        if existing and existing.expires_at > time.time():
            return existing.ip

        start = int(self.pool_start)
        end = int(self.pool_end)
        now = time.time()

        for ip_int in range(start, end + 1):
            candidate = str(ipaddress.IPv4Address(ip_int))
            if candidate in self.static_ips:
                continue
            in_use_by = self.dynamic_by_ip.get(candidate)
            if in_use_by and in_use_by != mac:
                continue

            self.dynamic_by_mac[mac] = Lease(
                ip=candidate,
                expires_at=now + self.lease_seconds,
            )
            self.dynamic_by_ip[candidate] = mac
            return candidate

        return None

    def allocate(self, mac: str, usb_path: Optional[str]) -> Optional[str]:
        if usb_path and usb_path in self.static_by_usb_path:
            return self.static_by_usb_path[usb_path]
        return self._allocate_dynamic(mac)

    def renew(self, mac: str, allocated_ip: str) -> None:
        lease = self.dynamic_by_mac.get(mac)
        if lease and lease.ip == allocated_ip:
            lease.expires_at = time.time() + self.lease_seconds

    def release(self, mac: str) -> None:
        lease = self.dynamic_by_mac.pop(mac, None)
        if lease:
            self.dynamic_by_ip.pop(lease.ip, None)


class GadgetDhcpServer:
    def __init__(self, config: dict[str, Any]) -> None:
        self.network = ipaddress.ip_network(config["subnet"], strict=False)
        self.router: Optional[str] = config.get("router")
        first_host = str(next(self.network.hosts()))
        self.server_ip = config.get("server_ip") or self.router or first_host
        self.dns_servers: list[str] = config.get("dns", [])
        self.lease_seconds = int(config.get("lease_time", 86400))
        self.static_by_usb_path: dict[str, str] = config.get("static_leases", {})

        pool = config["pool"]
        self.allocator = LeaseAllocator(
            pool_start=pool["start"],
            pool_end=pool["end"],
            lease_seconds=self.lease_seconds,
            static_by_usb_path=self.static_by_usb_path,
        )

        self.running = True
        self.usb_net_ifaces: list[str] = []
        self.iface_to_usb_path: dict[str, str] = {}
        self.iface_to_mac: dict[str, str] = {}

    def list_supported_usb_net_interfaces(self) -> list[str]:
        out: list[str] = []
        for entry in sorted(Path("/sys/class/net").iterdir()):
            if not entry.is_dir():
                continue
            ifname = entry.name
            driver_link = entry / "device" / "driver"
            if not driver_link.exists():
                continue
            try:
                driver_name = driver_link.resolve().name
            except OSError:
                continue
            if driver_name in SUPPORTED_USB_NET_DRIVERS:
                out.append(ifname)
        return out

    def resolve_usb_device_path(self, ifname: str) -> Optional[str]:
        device_link = Path("/sys/class/net") / ifname / "device"
        if not device_link.exists():
            return None

        try:
            resolved = device_link.resolve()
        except OSError:
            return None

        parts = list(resolved.parts)
        for part in reversed(parts):
            if USB_IFACE_RE.match(part):
                return part
        for part in reversed(parts):
            if USB_DEVICE_RE.match(part):
                return part
        return None

    def resolve_interface_mac(self, ifname: str) -> Optional[str]:
        addr_file = Path("/sys/class/net") / ifname / "address"
        if not addr_file.exists():
            return None

        try:
            mac = addr_file.read_text(encoding="utf-8").strip().lower()
        except OSError:
            return None

        if not mac:
            return None
        return mac

    def refresh_interfaces(self) -> None:
        current_ifaces = self.list_supported_usb_net_interfaces()

        current_map: dict[str, str] = {}
        current_mac_map: dict[str, str] = {}
        for ifname in current_ifaces:
            usb_path = self.resolve_usb_device_path(ifname)
            if usb_path:
                current_map[ifname] = usb_path
            mac = self.resolve_interface_mac(ifname)
            if mac:
                current_mac_map[ifname] = mac

        changed = (
            current_ifaces != self.usb_net_ifaces
            or current_map != self.iface_to_usb_path
            or current_mac_map != self.iface_to_mac
        )

        self.usb_net_ifaces = current_ifaces
        self.iface_to_usb_path = current_map
        self.iface_to_mac = current_mac_map

        if changed:
            LOG.debug("supported USB net interface set refreshed drivers=%s", sorted(SUPPORTED_USB_NET_DRIVERS))
            for ifname in self.usb_net_ifaces:
                LOG.debug(
                    "  iface=%s usb_path=%s",
                    ifname,
                    self.iface_to_usb_path.get(ifname, "<unresolved>"),
                )

    def resolve_ingress_interface(self, ingress_ifname: Optional[str], client_mac: str) -> Optional[str]:
        if ingress_ifname in self.usb_net_ifaces:
            return ingress_ifname

        for ifname, if_mac in self.iface_to_mac.items():
            if if_mac == client_mac:
                return ifname

        if len(self.usb_net_ifaces) == 1:
            return self.usb_net_ifaces[0]

        return None

    def _get_option(self, options: list[Any], key: str) -> Any:
        for option in options:
            if isinstance(option, tuple) and option[0] == key:
                return option[1]
        return None

    def _normalize_msg_type(self, msg_type: Any) -> Optional[str]:
        mapping = {
            1: "discover",
            2: "offer",
            3: "request",
            4: "decline",
            5: "ack",
            6: "nak",
            7: "release",
            8: "inform",
        }

        if isinstance(msg_type, bytes):
            try:
                msg_type = msg_type.decode("ascii", errors="ignore")
            except Exception:
                return None

        if isinstance(msg_type, int):
            return mapping.get(msg_type)

        if isinstance(msg_type, str):
            lowered = msg_type.lower()
            if lowered in mapping.values():
                return lowered
            try:
                return mapping.get(int(lowered))
            except ValueError:
                return None

        return None

    def _mac_from_packet(self, packet: Any) -> str:
        raw = bytes(packet[BOOTP].chaddr[:6])
        return ":".join(f"{b:02x}" for b in raw)

    def _build_common_options(self) -> list[Any]:
        options: list[Any] = [
            ("server_id", self.server_ip),
            ("subnet_mask", str(self.network.netmask)),
            ("lease_time", self.lease_seconds),
        ]
        if self.router:
            options.append(("router", self.router))
        if self.dns_servers:
            options.append(("name_server", self.dns_servers))
        return options

    def _send_raw_frame(self, egress_ifname: str, frame: Any, tag: str) -> bool:
        try:
            raw_frame = bytes(frame)
            with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL)) as tx_sock:
                tx_sock.sendto(raw_frame, (egress_ifname, 0))
            return True
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                LOG.warning("Send skipped; interface unavailable iface=%s %s", egress_ifname, tag)
                return False
            raise
        except ValueError as exc:
            LOG.warning("Send failed iface=%s %s: %s", egress_ifname, tag, exc)
            return False

    def _send_offer_or_ack(
        self,
        packet: Any,
        egress_ifname: str,
        offered_ip: str,
        msg_type: str,
    ) -> bool:
        bootp = packet[BOOTP]

        options: list[Any] = [("message-type", msg_type)]
        options.extend(self._build_common_options())
        options.append("end")

        try:
            server_mac = get_if_hwaddr(egress_ifname)
            reply = (
                Ether(src=server_mac, dst="ff:ff:ff:ff:ff:ff")
                / IP(src=self.server_ip, dst="255.255.255.255")
                / UDP(sport=67, dport=68)
                / BOOTP(
                    op=2,
                    xid=bootp.xid,
                    yiaddr=offered_ip,
                    siaddr=self.server_ip,
                    chaddr=bootp.chaddr,
                    flags=bootp.flags,
                )
                / DHCP(options=options)
            )
            return self._send_raw_frame(egress_ifname, reply, f"msg_type={msg_type}")
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                LOG.warning("Send skipped; interface unavailable iface=%s msg_type=%s", egress_ifname, msg_type)
                return False
            raise
        except ValueError as exc:
            LOG.warning("Send failed iface=%s msg_type=%s: %s", egress_ifname, msg_type, exc)
            return False

    def _send_nak(self, packet: Any, egress_ifname: str, reason: str) -> bool:
        bootp = packet[BOOTP]

        options: list[Any] = [
            ("message-type", "nak"),
            ("server_id", self.server_ip),
            ("message", reason),
            "end",
        ]

        try:
            server_mac = get_if_hwaddr(egress_ifname)
            reply = (
                Ether(src=server_mac, dst="ff:ff:ff:ff:ff:ff")
                / IP(src=self.server_ip, dst="255.255.255.255")
                / UDP(sport=67, dport=68)
                / BOOTP(
                    op=2,
                    xid=bootp.xid,
                    yiaddr="0.0.0.0",
                    siaddr=self.server_ip,
                    chaddr=bootp.chaddr,
                    flags=bootp.flags,
                )
                / DHCP(options=options)
            )
            return self._send_raw_frame(egress_ifname, reply, "msg_type=nak")
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                LOG.warning("NAK send skipped; interface unavailable iface=%s", egress_ifname)
                return False
            raise
        except ValueError as exc:
            LOG.warning("NAK send failed iface=%s: %s", egress_ifname, exc)
            return False

    def handle_packet(self, packet: Any) -> None:
        if not (packet.haslayer(DHCP) and packet.haslayer(BOOTP)):
            return

        dhcp_options = packet[DHCP].options
        msg_raw = self._get_option(dhcp_options, "message-type")
        msg_type = self._normalize_msg_type(msg_raw)
        if msg_type not in {"discover", "request", "release"}:
            return

        client_mac = self._mac_from_packet(packet)

        if msg_type == "discover":
            self.refresh_interfaces()

        ingress_ifname = getattr(packet, "sniffed_on", None)
        ifname = self.resolve_ingress_interface(ingress_ifname, client_mac)
        if not ifname:
            LOG.debug(
                "Ignore packet; ingress interface unresolved ingress_ifname=%s mac=%s",
                ingress_ifname,
                client_mac,
            )
            return

        usb_path = self.iface_to_usb_path.get(ifname)
        if usb_path is None:
            usb_path = self.resolve_usb_device_path(ifname)

        requested_ip = self._get_option(dhcp_options, "requested_addr")
        if isinstance(requested_ip, bytes):
            requested_ip = None

        if msg_type == "release":
            self.allocator.release(client_mac)
            LOG.info("RELEASE mac=%s", client_mac)
            return

        allocated_ip = self.allocator.allocate(client_mac, usb_path)
        if not allocated_ip:
            LOG.warning(
                "No free lease for mac=%s usb_path=%s on %s",
                client_mac,
                usb_path,
                ifname,
            )
            return

        if msg_type == "discover":
            sent = self._send_offer_or_ack(packet, ifname, allocated_ip, "offer")
            if sent:
                LOG.info(
                    "OFFER ip=%s mac=%s iface=%s usb_path=%s",
                    allocated_ip,
                    client_mac,
                    ifname,
                    usb_path,
                )
            else:
                LOG.warning(
                    "OFFER dropped ip=%s mac=%s iface=%s usb_path=%s",
                    allocated_ip,
                    client_mac,
                    ifname,
                    usb_path,
                )
            return

        static_ip = self.static_by_usb_path.get(usb_path, None) if usb_path else None
        if static_ip and requested_ip and requested_ip != static_ip:
            sent = self._send_nak(packet, ifname, "requested ip does not match static mapping")
            if sent:
                LOG.warning(
                    "NAK mac=%s requested=%s static=%s iface=%s usb_path=%s",
                    client_mac,
                    requested_ip,
                    static_ip,
                    ifname,
                    usb_path,
                )
            else:
                LOG.warning(
                    "NAK dropped mac=%s requested=%s static=%s iface=%s usb_path=%s",
                    client_mac,
                    requested_ip,
                    static_ip,
                    ifname,
                    usb_path,
                )
            return

        self.allocator.renew(client_mac, allocated_ip)
        sent = self._send_offer_or_ack(packet, ifname, allocated_ip, "ack")
        if sent:
            LOG.info(
                "ACK ip=%s mac=%s iface=%s usb_path=%s",
                allocated_ip,
                client_mac,
                ifname,
                usb_path,
            )
        else:
            LOG.warning(
                "ACK dropped ip=%s mac=%s iface=%s usb_path=%s",
                allocated_ip,
                client_mac,
                ifname,
                usb_path,
            )

    def _safe_process_packet(self, packet: Any) -> None:
        try:
            self.handle_packet(packet)
        except Exception:
            LOG.error("Unhandled exception in packet handler", exc_info=True)

    def _open_receiver(self, ifname: str) -> Optional[socket.socket]:
        try:
            recv_sock = socket.socket(
                socket.AF_PACKET,
                socket.SOCK_RAW,
                socket.htons(ETH_P_ALL),
            )
            recv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            recv_sock.bind((ifname, 0))
            recv_sock.setblocking(False)
            return recv_sock
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                LOG.info("Receiver bind deferred iface=%s; interface not ready", ifname)
                return None
            LOG.warning("Receiver bind failed iface=%s: %s", ifname, exc)
            return None

    def _sync_receivers(
        self,
        receivers: dict[str, socket.socket],
        fd_to_iface: dict[int, str],
    ) -> None:
        target_ifaces = set(self.usb_net_ifaces)

        for ifname in list(receivers.keys()):
            if ifname in target_ifaces:
                continue
            LOG.info("Closing receiver iface=%s", ifname)
            recv_sock = receivers.pop(ifname)
            fd = recv_sock.fileno()
            fd_to_iface.pop(fd, None)
            recv_sock.close()

        for ifname in sorted(target_ifaces):
            if ifname in receivers:
                continue
            recv_sock = self._open_receiver(ifname)
            if recv_sock is None:
                continue
            receivers[ifname] = recv_sock
            fd_to_iface[recv_sock.fileno()] = ifname
            LOG.info("Bound receiver iface=%s", ifname)

    def _close_all_receivers(
        self,
        receivers: dict[str, socket.socket],
        fd_to_iface: dict[int, str],
    ) -> None:
        for ifname in list(receivers.keys()):
            LOG.info("Closing receiver iface=%s", ifname)
            recv_sock = receivers.pop(ifname)
            fd = recv_sock.fileno()
            fd_to_iface.pop(fd, None)
            recv_sock.close()

    def _handle_receiver_data(self, ifname: str, recv_sock: socket.socket) -> None:
        while True:
            try:
                frame, _ = recv_sock.recvfrom(65535)
            except BlockingIOError:
                return
            except OSError as exc:
                if exc.errno == errno.ENODEV:
                    LOG.info("Receiver read stopped; interface vanished iface=%s", ifname)
                if exc.errno == errno.ENETDOWN:
                    LOG.info("Receiver read stopped; interface down iface=%s", ifname)
                else:
                    LOG.warning("Receiver read failed iface=%s: %s", ifname, exc)
                raise

            try:
                packet = Ether(frame)
            except Exception:
                continue

            setattr(packet, "sniffed_on", ifname)
            self._safe_process_packet(packet)

    def serve_forever(self) -> None:
        conf.checkIPaddr = False

        receivers: dict[str, socket.socket] = {}
        fd_to_iface: dict[int, str] = {}
        monitor = pyudev.Monitor.from_netlink(pyudev.Context())
        monitor.filter_by(subsystem="net")
        monitor.start()
        monitor_fd = monitor.fileno()
        last_resync = time.monotonic()

        try:
            self.refresh_interfaces()
            self._sync_receivers(receivers, fd_to_iface)

            while self.running:
                try:
                    read_fds: list[Any] = [monitor_fd]
                    read_fds.extend(receivers.values())
                    ready, _, _ = select.select(read_fds, [], [], 1.0)
                except OSError as exc:
                    if exc.errno == errno.EINTR:
                        continue
                    LOG.error("udev monitor select failed: %s", exc)
                    time.sleep(1)
                    continue

                changed = False
                for fd in ready:
                    if fd == monitor_fd:
                        while True:
                            device = monitor.poll(timeout=0)
                            if device is None:
                                break

                            action = device.action or "unknown"
                            ifname = device.get("INTERFACE")
                            LOG.debug("udev event action=%s interface=%s", action, ifname)
                            changed = True
                        continue

                    recv_sock = fd
                    sock_fd = recv_sock.fileno()
                    ifname = fd_to_iface.get(sock_fd)
                    if not ifname:
                        continue

                    try:
                        self._handle_receiver_data(ifname, recv_sock)
                    except OSError:
                        stale = receivers.pop(ifname, None)
                        if stale is not None:
                            stale_fd = stale.fileno()
                            fd_to_iface.pop(stale_fd, None)
                            stale.close()
                        changed = True

                if not ready:
                    now = time.monotonic()
                    if now - last_resync >= 3.0:
                        self.refresh_interfaces()
                        self._sync_receivers(receivers, fd_to_iface)
                        last_resync = now
                    continue

                if not changed:
                    continue

                self.refresh_interfaces()
                self._sync_receivers(receivers, fd_to_iface)
                last_resync = time.monotonic()
        finally:
            self._close_all_receivers(receivers, fd_to_iface)


def load_config(config_path: str) -> dict[str, Any]:
    path = Path(config_path)
    if not path.exists():
        raise ConfigError(f"config not found: {config_path}")

    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)

    required = ["subnet", "pool"]
    for key in required:
        if key not in data:
            raise ConfigError(f"missing required config field: {key}")

    network = ipaddress.ip_network(data["subnet"], strict=False)
    if "router" in data:
        router = ipaddress.ip_address(data["router"])
        if router not in network:
            raise ConfigError("router must be inside subnet")

    if "server_ip" in data:
        server_ip = ipaddress.ip_address(data["server_ip"])
        if server_ip not in network:
            raise ConfigError("server_ip must be inside subnet")

    pool = data["pool"]
    for key in ["start", "end"]:
        if key not in pool:
            raise ConfigError(f"pool.{key} is required")

    pool_start = ipaddress.ip_address(pool["start"])
    pool_end = ipaddress.ip_address(pool["end"])
    if pool_start not in network or pool_end not in network:
        raise ConfigError("pool range must be inside subnet")
    if int(pool_end) < int(pool_start):
        raise ConfigError("pool.end must be >= pool.start")

    lease_time = int(data.get("lease_time", 86400))
    if lease_time <= 0:
        raise ConfigError("lease_time must be > 0")

    dns_servers = data.get("dns", [])
    if dns_servers is None:
        dns_servers = []
    if not isinstance(dns_servers, list):
        raise ConfigError("dns must be a list of IPv4 addresses when provided")
    normalized_dns: list[str] = []
    for dns_ip in dns_servers:
        dns_addr = ipaddress.ip_address(dns_ip)
        if not isinstance(dns_addr, ipaddress.IPv4Address):
            raise ConfigError("dns entries must be IPv4 addresses")
        normalized_dns.append(str(dns_addr))
    data["dns"] = normalized_dns

    static_leases = data.get("static_leases", {})
    if not isinstance(static_leases, dict):
        raise ConfigError("static_leases must be an object mapping usb path to ip")

    seen_ips: set[str] = set()
    for usb_path, ip_str in static_leases.items():
        if not isinstance(usb_path, str) or not usb_path:
            raise ConfigError("static lease keys must be non-empty usb path strings")
        ip_obj = ipaddress.ip_address(ip_str)
        if ip_obj not in network:
            raise ConfigError(
                f"static lease {usb_path}={ip_str} is outside subnet {network}"
            )
        if ip_str in seen_ips:
            raise ConfigError(f"duplicate static lease ip: {ip_str}")
        seen_ips.add(ip_str)

    return data


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Simple DHCP server for USB gadget cdc_ether/cdc_eem clients"
    )
    parser.add_argument(
        "--config",
        required=True,
        help="Path to JSON configuration file",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging level",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    try:
        config = load_config(args.config)
    except (ConfigError, json.JSONDecodeError) as exc:
        LOG.error("Invalid config: %s", exc)
        return 2

    server = GadgetDhcpServer(config=config)

    def _stop(_signum: int, _frame: Any) -> None:
        LOG.info("Stopping server")
        server.running = False

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    LOG.info("Starting DHCP server for drivers=%s", sorted(SUPPORTED_USB_NET_DRIVERS))
    LOG.info("Static usb-path mappings: %d", len(server.static_by_usb_path))
    LOG.debug("Requires root or capabilities for raw packet capture/send")

    try:
        server.serve_forever()
    except PermissionError:
        LOG.error("Permission denied; run as root or add NET_RAW capability")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
