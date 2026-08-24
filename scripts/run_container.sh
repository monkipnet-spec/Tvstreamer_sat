#!/usr/bin/env bash
set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
    echo "docker was not found in PATH" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${IMAGE_NAME:-tvstreammersat5:202.8}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/tvstreammersat5-config.json}"
DETACH="${DETACH:-0}"
CONTAINER_NAME="${CONTAINER_NAME:-tvstreammersat5}"
RESTART_POLICY="${RESTART_POLICY:-unless-stopped}"

if [[ "${DETACH}" != "0" && "${DETACH}" != "1" ]]; then
    echo "DETACH must be 0 or 1" >&2
    exit 2
fi

if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "Config file not found: ${CONFIG_FILE}" >&2
    echo "Create it first or set CONFIG_FILE=/absolute/path/to/tvstreammersat5-config.json" >&2
    exit 1
fi

# Docker bind sources should be absolute paths. realpath is part of coreutils on
# supported Ubuntu/Debian hosts.
CONFIG_FILE="$(realpath "${CONFIG_FILE}")"
DATA_DIR="$(dirname "${CONFIG_FILE}")"
CONFIG_BASENAME="$(basename "${CONFIG_FILE}")"

VOLUME_ARGS=(-v "${DATA_DIR}:/data")
if [[ "${CONFIG_BASENAME}" != "tvstreammersat5-config.json" ]]; then
    # Keep the whole data directory mounted so subscriber/backup files persist,
    # while exposing a custom config filename under the canonical app name.
    VOLUME_ARGS+=(-v "${CONFIG_FILE}:/data/tvstreammersat5-config.json")
fi

DEVICE_ARGS=()
if [[ -d /dev/dvb ]]; then
    while IFS= read -r device; do
        DEVICE_ARGS+=(--device "${device}:${device}")
    done < <(find /dev/dvb -type c -print | sort)
fi

RUN_ARGS=(
    --init
    --network host
    "${DEVICE_ARGS[@]}"
    "${VOLUME_ARGS[@]}"
    -w /data
    -e "GST_DEBUG=${GST_DEBUG:-1}"
)

if [[ "${DETACH}" == "1" ]]; then
    RUN_ARGS+=(
        -d
        --name "${CONTAINER_NAME}"
        --restart "${RESTART_POLICY}"
    )
else
    RUN_ARGS+=(--rm -it)
fi

exec docker run "${RUN_ARGS[@]}" "${IMAGE_NAME}"
