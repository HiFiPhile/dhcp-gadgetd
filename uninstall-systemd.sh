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
