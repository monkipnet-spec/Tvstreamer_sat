#!/usr/bin/env bash
set -euo pipefail

SRC="${1:?vendored source directory is required}"
OUT="${2:?output directory is required}"
WORK="$OUT/src"
BUILD="$OUT/cmake-build"
BIN_OUT="$OUT/oscam-mini"

for cmd in cmake gcc make; do
  command -v "$cmd" >/dev/null || { echo "Missing build dependency: $cmd" >&2; exit 1; }
done

if [[ ! -f "$SRC/config.sh" || ! -f "$SRC/CMakeLists.txt" ]]; then
  echo "ERROR: OSCam source is incomplete in: $SRC" >&2
  echo "Expected at least config.sh and CMakeLists.txt." >&2
  echo "Commit the complete third_party/oscam-mini tree to the repository." >&2
  exit 2
fi

rm -rf "$WORK" "$BUILD"
mkdir -p "$OUT"
cp -a "$SRC" "$WORK"
mkdir -p "$WORK/Distribution" "$WORK/webif"
chmod +x "$WORK/config.sh"

cd "$WORK"
./config.sh --disable all
./config.sh --enable MODULE_NEWCAMD READER_IRDETO READER_VIACCESS CARDREADER_PHOENIX

printf '\nEnabled OSCam-mini modules:\n'
./config.sh --show-enabled all

# Build via OSCam's CMakeLists instead of relying on the upstream root Makefile.
# This also avoids failures when an archive was unpacked through Windows and file
# permissions or the Makefile were lost.
cmake -S "$WORK" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCS_CONFDIR=/opt/TVStreammerSAT5/oscam-mini/config
cmake --build "$BUILD" --target oscam -j"$(nproc)"

if [[ ! -x "$BUILD/oscam" ]]; then
  echo "ERROR: OSCam binary was not produced at $BUILD/oscam" >&2
  exit 3
fi

install -m0755 "$BUILD/oscam" "$BIN_OUT"
echo "OSCam-mini built: $BIN_OUT"
