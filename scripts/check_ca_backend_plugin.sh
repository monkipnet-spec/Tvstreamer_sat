#!/usr/bin/env bash
set -euo pipefail

plugin="${1:-}"
if [[ -z "$plugin" || ! -f "$plugin" ]]; then
  echo "Usage: $0 /path/to/backend.so" >&2
  exit 2
fi

entry="tvstreammersat5_ca_backend_get_api_v1"
if ! command -v nm >/dev/null 2>&1; then
  echo "nm is required (binutils)" >&2
  exit 3
fi

if ! nm -D --defined-only "$plugin" 2>/dev/null | grep -q "[[:space:]]${entry}$"; then
  echo "ERROR: required ABI entry point is missing: ${entry}" >&2
  exit 4
fi

echo "OK: ${plugin} exports ${entry}"
if command -v ldd >/dev/null 2>&1; then
  echo "Dependencies:"
  ldd "$plugin" || true
fi
