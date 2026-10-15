#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="dhcp-gadget.service"
APP_DIR="/opt/dhcp-gadget"
CONFIG_DIR="/etc/dhcp-gadget"
ENV_FILE="/etc/default/dhcp-gadget"
SYSTEMD_UNIT="/etc/systemd/system/${SERVICE_NAME}"
BINARY_NAME="dhcp-gadgetd"
PURGE="${1:-}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script must be run as root." >&2
  echo "Use: sudo ./uninstall-systemd.sh [--purge]" >&2
  exit 1
fi

if systemctl list-unit-files | grep -q "^${SERVICE_NAME}"; then
  systemctl disable --now "${SERVICE_NAME}" || true
fi

# The daemon keeps its bridge across restarts; remove it with the service.
BRIDGE="gadget0"
if [[ -f "${CONFIG_DIR}/config.conf" ]]; then
  configured_bridge="$(awk -F= '$1 ~ /^[[:space:]]*bridge[[:space:]]*$/ { value = $2 }
    END { gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); print value }' "${CONFIG_DIR}/config.conf")"
  if [[ -n "${configured_bridge}" ]]; then
    BRIDGE="${configured_bridge}"
  fi
fi
if [[ -d "/sys/class/net/${BRIDGE}/bridge" ]]; then
  ip link delete dev "${BRIDGE}"
  echo "Deleted bridge ${BRIDGE}"
fi

rm -f /etc/udev/rules.d/90-dhcp-gadget.rules
udevadm control --reload
# Hand already plugged-in gadget ports back to the host network managers.
udevadm trigger --action=change --subsystem-match=net \
  --property-match=ID_NET_DRIVER=cdc_ether --property-match=ID_NET_DRIVER=cdc_eem

rm -f /etc/dbus-1/system.d/org.dhcp_gadget1.conf
busctl call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
rm -f "${SYSTEMD_UNIT}"
systemctl daemon-reload
systemctl reset-failed || true

rm -f "${APP_DIR}/${BINARY_NAME}"
rmdir "${APP_DIR}" 2>/dev/null || true

if [[ "${PURGE}" == "--purge" ]]; then
  rm -rf "${CONFIG_DIR}"
  rm -f "${ENV_FILE}"
  echo "Purged ${CONFIG_DIR} and ${ENV_FILE}"
else
  echo "Kept ${CONFIG_DIR} and ${ENV_FILE}"
fi

echo "Uninstalled ${SERVICE_NAME}"
