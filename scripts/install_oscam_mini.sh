#!/usr/bin/env bash
set -euo pipefail
ROOT="/opt/TVStreammerSAT5/oscam-mini"
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
install -m0644 "${ROOT}/oscam-mini.service" /etc/systemd/system/oscam-mini.service
systemctl daemon-reload
if systemctl list-unit-files oscam.service >/dev/null 2>&1; then
  systemctl disable --now oscam.service || true
fi
systemctl enable --now oscam-mini.service
systemctl --no-pager --full status oscam-mini.service || true
