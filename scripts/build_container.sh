#!/usr/bin/env bash
set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
    echo "docker was not found in PATH" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${IMAGE_NAME:-tvstreammersat5:202.9}"

# Use the repository root even when this script is launched from another cwd.
docker build --pull -f "${ROOT_DIR}/Dockerfile" -t "${IMAGE_NAME}" "${ROOT_DIR}"
echo "Built ${IMAGE_NAME}"
