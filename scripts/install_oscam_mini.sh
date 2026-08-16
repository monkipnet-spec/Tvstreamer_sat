#!/usr/bin/env bash
set -euo pipefail

ROOT=/opt/TVStreammerSAT5/oscam-mini
CFG="$ROOT/config"
DEFAULT="$ROOT/default-config"
UNIT_SRC="$ROOT/oscam-mini.service"
UNIT_DST=/etc/systemd/system/oscam-mini.service

[[ $EUID -eq 0 ]] || { echo "Run with sudo" >&2; exit 1; }
[[ -x "$ROOT/oscam-mini" ]] || { echo "Missing $ROOT/oscam-mini. Run: sudo cmake --install build" >&2; exit 2; }

mkdir -p "$CFG"
for f in oscam.conf oscam.server oscam.user; do
  if [[ ! -e "$CFG/$f" && -e "$DEFAULT/$f" ]]; then
    install -m0600 "$DEFAULT/$f" "$CFG/$f"
    echo "Created $CFG/$f from default configuration"
  else
    echo "Keeping existing $CFG/$f"
  fi
done

# cmake --install already installs the unit to /etc/systemd/system. Keep this
# fallback so older installations can still be repaired by running this script.
if [[ ! -f "$UNIT_DST" ]]; then
  [[ -f "$UNIT_SRC" ]] || { echo "Missing systemd unit: $UNIT_SRC" >&2; exit 3; }
  install -m0644 "$UNIT_SRC" "$UNIT_DST"
fi

systemctl daemon-reload

# Legacy OSCam must not own the same Phoenix devices at the same time.
if systemctl list-unit-files oscam.service >/dev/null 2>&1; then
  systemctl disable --now oscam.service || true
fi
pkill -x oscam 2>/dev/null || true

systemctl enable --now oscam-mini.service
systemctl --no-pager --full status oscam-mini.service || true
