#!/usr/bin/env bash
set -euo pipefail
OUT="${1:?output dir}"; REPO="${2:-https://github.com/gfto/oscam.git}"; REV="${3:-2780c48789c8e1427df4078ea9b06e0b51594bbc}"
SRC="${OUT}/src"; mkdir -p "${OUT}"
if [[ ! -d "${SRC}/.git" ]]; then rm -rf "${SRC}"; git clone "${REPO}" "${SRC}"; fi
git -C "${SRC}" fetch --all --tags --prune
git -C "${SRC}" checkout --force "${REV}"
git -C "${SRC}" reset --hard "${REV}"
git -C "${SRC}" clean -fdx
cd "${SRC}"
./config.sh --disable all
./config.sh --enable MODULE_NEWCAMD READER_IRDETO READER_VIACCESS CARDREADER_PHOENIX
./config.sh --show-enabled all
make clean
make -j"$(nproc)"
BIN="$(find Distribution . -maxdepth 3 -type f -perm -111 -name 'oscam*' | head -n1)"
test -n "${BIN}"
install -m0755 "${BIN}" "${OUT}/oscam-mini"
echo "OSCam-mini built: ${OUT}/oscam-mini"
