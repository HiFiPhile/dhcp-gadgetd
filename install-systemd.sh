#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="dhcp-gadget.service"
APP_DIR="/opt/dhcp-gadget"
CONFIG_DIR="/etc/dhcp-gadget"
ENV_FILE="/etc/default/dhcp-gadget"
SYSTEMD_UNIT="/etc/systemd/system/${SERVICE_NAME}"
BINARY_NAME="dhcp-gadgetd"

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script must be run as root." >&2
  echo "Use: sudo ./install-systemd.sh" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "${SCRIPT_DIR}/${BINARY_NAME}" ]]; then
  echo "${BINARY_NAME} not found next to installer script." >&2
  echo "Build first: make" >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/dhcp-gadget.service" ]]; then
  echo "dhcp-gadget.service not found next to installer script." >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/dhcp-gadget.env" ]]; then
  echo "dhcp-gadget.env not found next to installer script." >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/config.example.conf" ]]; then
  echo "config.example.conf not found next to installer script." >&2
  exit 1
fi

for file in dhcp-gadget.sysusers org.dhcp_gadget1.conf 90-dhcp-gadget.rules; do
  if [[ ! -f "${SCRIPT_DIR}/${file}" ]]; then
    echo "${file} not found next to installer script." >&2
    exit 1
  fi
done

# Stop before creating the permanent account: the old DynamicUser account is
# visible through NSS while running, causing sysusers to skip account creation.
unit_load_state="$(systemctl show --property=LoadState --value "${SERVICE_NAME}")"
if [[ "${unit_load_state}" != "not-found" ]]; then
  systemctl stop "${SERVICE_NAME}"
fi

systemd-sysusers "${SCRIPT_DIR}/dhcp-gadget.sysusers"
if ! getent -s files passwd dhcp-gadget >/dev/null ||
   ! getent -s files group dhcp-gadget >/dev/null; then
  echo "Permanent dhcp-gadget user/group was not created; installation stopped." >&2
  exit 1
fi
install -d -m 0755 /etc/dbus-1/system.d
install -m 0644 "${SCRIPT_DIR}/org.dhcp_gadget1.conf" /etc/dbus-1/system.d/org.dhcp_gadget1.conf
busctl call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig

install -m 0644 "${SCRIPT_DIR}/90-dhcp-gadget.rules" /etc/udev/rules.d/90-dhcp-gadget.rules
udevadm control --reload
# Apply the rule to gadget ports that are already plugged in.
udevadm trigger --action=change --subsystem-match=net \
  --property-match=ID_NET_DRIVER=cdc_ether --property-match=ID_NET_DRIVER=cdc_eem

install -d -m 0755 "${APP_DIR}"
install -d -m 0755 "${CONFIG_DIR}"

install -m 0755 "${SCRIPT_DIR}/${BINARY_NAME}" "${APP_DIR}/${BINARY_NAME}"

if [[ ! -f "${CONFIG_DIR}/config.conf" ]]; then
  install -m 0644 "${SCRIPT_DIR}/config.example.conf" "${CONFIG_DIR}/config.conf"
  echo "Created default config at ${CONFIG_DIR}/config.conf"
else
  echo "Keeping existing config at ${CONFIG_DIR}/config.conf"
fi

if [[ ! -f "${ENV_FILE}" ]]; then
  install -m 0644 "${SCRIPT_DIR}/dhcp-gadget.env" "${ENV_FILE}"
  echo "Created ${ENV_FILE}"
else
  echo "Keeping existing ${ENV_FILE}"
fi

install -m 0644 "${SCRIPT_DIR}/dhcp-gadget.service" "${SYSTEMD_UNIT}"

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"
systemctl reset-failed "${SERVICE_NAME}"
systemctl restart "${SERVICE_NAME}"

echo "Installed and started ${SERVICE_NAME}"
echo "Edit config: ${CONFIG_DIR}/config.conf"
echo "Service env: ${ENV_FILE}"
echo "Check status: systemctl status ${SERVICE_NAME}"
