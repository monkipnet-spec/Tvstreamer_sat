#!/usr/bin/env bash
set -euo pipefail
ROOT=/opt/TVStreammerSAT5/oscam-mini
CFG="$ROOT/config"
DEFAULT="$ROOT/default-config"
[[ $EUID -eq 0 ]] || { echo "Run with sudo" >&2; exit 1; }
mkdir -p "$CFG"
for f in oscam.conf oscam.server oscam.user; do
  if [[ ! -e "$CFG/$f" && -e "$DEFAULT/$f" ]]; then
    install -m0600 "$DEFAULT/$f" "$CFG/$f"
  fi
done
install -m0644 "$ROOT/oscam-mini.service" /etc/systemd/system/oscam-mini.service
systemctl daemon-reload
if systemctl list-unit-files oscam.service >/dev/null 2>&1; then
  systemctl disable --now oscam.service || true
fi
systemctl enable --now oscam-mini.service
systemctl --no-pager --full status oscam-mini.service || true
