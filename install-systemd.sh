#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="dhcp-gadget.service"
APP_DIR="/opt/dhcp-gadget"
CONFIG_DIR="/etc/dhcp-gadget"
ENV_FILE="/etc/default/dhcp-gadget"
SYSTEMD_UNIT="/etc/systemd/system/${SERVICE_NAME}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script must be run as root." >&2
  echo "Use: sudo ./install-systemd.sh" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "${SCRIPT_DIR}/dhcp_server.py" ]]; then
  echo "dhcp_server.py not found next to installer script." >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/dhcp-gadget.service" ]]; then
  echo "dhcp-gadget.service not found next to installer script." >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/config.example.json" ]]; then
  echo "config.example.json not found next to installer script." >&2
  exit 1
fi

install -d -m 0755 "${APP_DIR}"
install -d -m 0755 "${CONFIG_DIR}"

install -m 0644 "${SCRIPT_DIR}/dhcp_server.py" "${APP_DIR}/dhcp_server.py"
if [[ -f "${SCRIPT_DIR}/requirements.txt" ]]; then
  install -m 0644 "${SCRIPT_DIR}/requirements.txt" "${APP_DIR}/requirements.txt"
fi

if [[ ! -f "${CONFIG_DIR}/config.json" ]]; then
  install -m 0644 "${SCRIPT_DIR}/config.example.json" "${CONFIG_DIR}/config.json"
  echo "Created default config at ${CONFIG_DIR}/config.json"
else
  echo "Keeping existing config at ${CONFIG_DIR}/config.json"
fi

if [[ ! -f "${ENV_FILE}" ]]; then
  cat > "${ENV_FILE}" <<'EOF'
# Environment overrides for dhcp-gadget.service

CONFIG=/etc/dhcp-gadget/config.json
LOG_LEVEL=INFO
EOF
  echo "Created ${ENV_FILE}"
else
  echo "Keeping existing ${ENV_FILE}"
fi

install -m 0644 "${SCRIPT_DIR}/dhcp-gadget.service" "${SYSTEMD_UNIT}"

systemctl daemon-reload
systemctl enable --now "${SERVICE_NAME}"

echo "Installed and started ${SERVICE_NAME}"
echo "Edit config: ${CONFIG_DIR}/config.json"
echo "Service env: ${ENV_FILE}"
echo "Check status: systemctl status ${SERVICE_NAME}"
