#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$ROOT/third_party/oscam-mini"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --depth 1 https://github.com/gfto/oscam.git "$TMP/oscam"
rm -rf "$DST"
mkdir -p "$DST"
cp -a "$TMP/oscam/." "$DST/"
rm -rf "$DST/.git"
printf '%s\n' 'https://github.com/gfto/oscam.git' > "$DST/UPSTREAM_URL"
git -C "$TMP/oscam" rev-parse HEAD > "$DST/UPSTREAM_COMMIT"
echo "Vendored OSCam source into $DST"
