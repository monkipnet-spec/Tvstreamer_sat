#!/usr/bin/env bash
set -Eeuo pipefail

# Removes TVStreammerSAT5 binaries/services and dependencies that were newly
# installed by install_tvstreammersat5_binary.sh.
#
# By default user configuration and backup files are preserved in the install
# directory. Pass --purge-data to delete the entire application directory too.

APP_NAME="TVStreammerSAT5"
SERVICE_NAME="tvstreammersat5.service"
OSCAM_SERVICE_NAME="oscam-mini.service"
STATE_DIR="${TVS_INSTALLER_STATE_DIR:-/var/lib/tvstreammersat5-installer}"
INSTALL_DIR="${TVS_INSTALL_DIR:-/opt/TVStreammerSAT5}"
PURGE_DATA=0
PURGE_DEPS=1
DRY_RUN=0

usage() {
    cat <<'USAGE'
Usage: sudo ./uninstall_tvstreammersat5.sh [options]

Options:
  --purge-data       Also delete configs, UI key, subscribers and backup-files
  --keep-deps        Keep Ubuntu packages installed by the binary installer
  --install-dir DIR  Application directory if non-default
  --dry-run          Show what would be removed without changing the system
  -h, --help         Show this help

Default behavior removes program binaries/services and installer-added packages,
but preserves user data/configuration under /opt/TVStreammerSAT5.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --purge-data)
            PURGE_DATA=1
            shift
            ;;
        --keep-deps)
            PURGE_DEPS=0
            shift
            ;;
        --install-dir)
            [[ $# -ge 2 ]] || { echo "--install-dir requires a directory" >&2; exit 2; }
            INSTALL_DIR="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "${EUID}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        args=(--install-dir "$INSTALL_DIR")
        [[ "$PURGE_DATA" -eq 1 ]] && args+=(--purge-data)
        [[ "$PURGE_DEPS" -eq 0 ]] && args+=(--keep-deps)
        [[ "$DRY_RUN" -eq 1 ]] && args+=(--dry-run)
        exec sudo -E bash "$0" "${args[@]}"
    fi
    echo "Run as root or install sudo first." >&2
    exit 1
fi

run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf '+ '
        printf '%q ' "$@"
        printf '\n'
    else
        "$@"
    fi
}

if [[ -f "$STATE_DIR/install-dir.txt" && "$INSTALL_DIR" == "/opt/TVStreammerSAT5" ]]; then
    RECORDED_DIR="$(head -n1 "$STATE_DIR/install-dir.txt" 2>/dev/null || true)"
    [[ -n "$RECORDED_DIR" ]] && INSTALL_DIR="$RECORDED_DIR"
fi

echo "Removing TVStreammerSAT5 from: $INSTALL_DIR"
[[ "$PURGE_DATA" -eq 1 ]] && echo "User data/configuration will also be deleted."

for service in "$SERVICE_NAME" "$OSCAM_SERVICE_NAME"; do
    if systemctl list-unit-files "$service" >/dev/null 2>&1 || [[ -f "/etc/systemd/system/$service" ]]; then
        run systemctl disable --now "$service" || true
    fi
done

# These units are TVStreammerSAT5-specific. Remove them even if an older manual
# installation created them, otherwise systemd would retain broken ExecStart paths.
run rm -f "/etc/systemd/system/$SERVICE_NAME"
run rm -f "/etc/systemd/system/$OSCAM_SERVICE_NAME"
run systemctl daemon-reload
run systemctl reset-failed "$SERVICE_NAME" "$OSCAM_SERVICE_NAME" || true

if [[ "$PURGE_DATA" -eq 1 ]]; then
    run rm -rf -- "$INSTALL_DIR"
    # Older CMake versions installed the CA plugin in a lower-case path.
    if [[ "$INSTALL_DIR" != "/opt/tvstreammersat5" ]]; then
        run rm -rf -- "/opt/tvstreammersat5/ca-plugins"
        if [[ -d /opt/tvstreammersat5 ]] && [[ -z "$(find /opt/tvstreammersat5 -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
            run rmdir /opt/tvstreammersat5 || true
        fi
    fi
else
    run rm -f -- "$INSTALL_DIR/$APP_NAME"
    run rm -rf -- "$INSTALL_DIR/ca-plugins"
    run rm -f -- "$INSTALL_DIR/oscam-mini/oscam-mini"
    run rm -rf -- "$INSTALL_DIR/oscam-mini/default-config"
    # Keep only mutable OSCam config if it exists; remove empty directories.
    if [[ -d "$INSTALL_DIR/oscam-mini" ]]; then
        if [[ -z "$(find "$INSTALL_DIR/oscam-mini" -mindepth 1 -maxdepth 1 ! -name config -print -quit 2>/dev/null)" ]]; then
            if [[ ! -d "$INSTALL_DIR/oscam-mini/config" ]] || \
               [[ -z "$(find "$INSTALL_DIR/oscam-mini/config" -mindepth 1 -print -quit 2>/dev/null)" ]]; then
                run rm -rf -- "$INSTALL_DIR/oscam-mini"
            fi
        fi
    fi
fi

if [[ "$PURGE_DEPS" -eq 1 && -s "$STATE_DIR/installed-packages.txt" ]]; then
    mapfile -t PACKAGES < <(grep -Ev '^\s*(#|$)' "$STATE_DIR/installed-packages.txt" | sort -u)
    INSTALLED_NOW=()
    for pkg in "${PACKAGES[@]}"; do
        if dpkg-query -W -f='${db:Status-Abbrev}' "$pkg" 2>/dev/null | grep -q '^ii'; then
            INSTALLED_NOW+=("$pkg")
        fi
    done

    if [[ "${#INSTALLED_NOW[@]}" -gt 0 ]]; then
        echo "Removing ${#INSTALLED_NOW[@]} package(s) that were absent before the installer ran..."
        # These are exactly the package delta recorded at installation time.
        # apt still performs dependency checks; it will not leave broken Depends.
        run env DEBIAN_FRONTEND=noninteractive apt-get purge -y "${INSTALLED_NOW[@]}"
    fi
else
    echo "Dependency removal skipped (no installer manifest or --keep-deps used)."
fi

run rm -rf -- "$STATE_DIR"

if [[ "$DRY_RUN" -eq 0 ]]; then
    echo
    echo "TVStreammerSAT5 binaries and services removed."
    if [[ "$PURGE_DATA" -eq 1 ]]; then
        echo "User data/configuration removed: $INSTALL_DIR"
    else
        echo "User data/configuration preserved in: $INSTALL_DIR"
        echo "For a complete data purge run again with: --purge-data"
    fi
else
    echo "Dry run complete; no changes were made."
fi
