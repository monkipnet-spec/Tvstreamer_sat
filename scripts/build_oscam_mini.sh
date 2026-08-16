#!/usr/bin/env bash
set -euo pipefail
SRC="${1:?vendored source directory is required}"
OUT="${2:?output directory is required}"
WORK="$OUT/src"
BIN_OUT="$OUT/oscam-mini"

for cmd in make gcc; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 1; }; done
[[ -f "$SRC/config.sh" && -f "$SRC/Makefile" ]] || {
  echo "OSCam source is not vendored in $SRC" >&2
  echo "Run scripts/vendor_oscam_mini.sh (or .ps1 on Windows), commit third_party/oscam-mini, then build again." >&2
  exit 2
}
rm -rf "$WORK"
mkdir -p "$OUT"
cp -a "$SRC" "$WORK"
mkdir -p "$WORK/Distribution" "$WORK/webif"
cd "$WORK"
chmod +x ./config.sh
./config.sh --disable all
./config.sh --enable MODULE_NEWCAMD READER_IRDETO READER_VIACCESS CARDREADER_PHOENIX
printf '\nEnabled OSCam-mini modules:\n'
./config.sh --show-enabled all
make clean
make -j"$(nproc)"
BIN="$(find Distribution -maxdepth 1 -type f -perm -111 -name 'oscam*' | head -n1)"
[[ -n "$BIN" ]] || { echo "OSCam binary not found" >&2; exit 3; }
install -m0755 "$BIN" "$BIN_OUT"
echo "OSCam-mini built: $BIN_OUT"
