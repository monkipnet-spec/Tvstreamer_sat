#!/usr/bin/env bash
set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
    echo "docker was not found in PATH" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${IMAGE_NAME:-tvstreammersat5:202.25}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/tvstreammersat5-config.json}"
DETACH="${DETACH:-0}"
CONTAINER_NAME="${CONTAINER_NAME:-tvstreammersat5}"
RESTART_POLICY="${RESTART_POLICY:-unless-stopped}"
RECREATE="${RECREATE:-0}"

if [[ "${DETACH}" != "0" && "${DETACH}" != "1" ]]; then
    echo "DETACH must be 0 or 1" >&2
    exit 2
fi

if [[ "${RECREATE}" != "0" && "${RECREATE}" != "1" ]]; then
    echo "RECREATE must be 0 or 1" >&2
    exit 2
fi

# Docker bind sources should be absolute paths. realpath is part of coreutils on
# supported Ubuntu/Debian hosts.
CONFIG_FILE="$(realpath -m "${CONFIG_FILE}")"
DATA_DIR="$(dirname "${CONFIG_FILE}")"
CONFIG_BASENAME="$(basename "${CONFIG_FILE}")"

if [[ -e "${CONFIG_FILE}" && ! -f "${CONFIG_FILE}" ]]; then
    echo "Config path is not a regular file: ${CONFIG_FILE}" >&2
    exit 1
fi

if [[ ! -f "${CONFIG_FILE}" && "${CONFIG_BASENAME}" != "tvstreammersat5-config.json" ]]; then
    echo "Custom config file not found: ${CONFIG_FILE}" >&2
    echo "Create it first, or use the standard filename tvstreammersat5-config.json" >&2
    exit 1
fi

mkdir -p "${DATA_DIR}"
if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "Config file will be created on first start: ${CONFIG_FILE}"
fi

if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo "Docker image not found: ${IMAGE_NAME}" >&2
    echo "Build it first with: docker build --pull -t ${IMAGE_NAME} ." >&2
    exit 1
fi

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

if [[ -n "${TVS_UDP_STARTUP_BUFFER_MS:-}" ]]; then
    RUN_ARGS+=(-e "TVS_UDP_STARTUP_BUFFER_MS=${TVS_UDP_STARTUP_BUFFER_MS}")
fi
if [[ -n "${TVS_UDP_FORCE_SYNTHETIC_PCR:-}" ]]; then
    RUN_ARGS+=(-e "TVS_UDP_FORCE_SYNTHETIC_PCR=${TVS_UDP_FORCE_SYNTHETIC_PCR}")
fi

if [[ "${DETACH}" == "1" ]]; then
    if [[ "${RECREATE}" == "1" ]] &&
       docker container inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
        echo "Removing existing container: ${CONTAINER_NAME}"
        docker rm -f "${CONTAINER_NAME}" >/dev/null
    fi
    RUN_ARGS+=(
        -d
        --name "${CONTAINER_NAME}"
        --restart "${RESTART_POLICY}"
    )
    CONTAINER_ID="$(docker run "${RUN_ARGS[@]}" "${IMAGE_NAME}")"
    echo "Container started: ${CONTAINER_NAME} (${CONTAINER_ID})"
    docker ps --filter "name=^${CONTAINER_NAME}$" \
        --format 'table {{.Names}}\t{{.Status}}\t{{.Image}}'
else
    RUN_ARGS+=(--rm -it)
    exec docker run "${RUN_ARGS[@]}" "${IMAGE_NAME}"
fi
