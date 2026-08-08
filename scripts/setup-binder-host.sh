#!/usr/bin/env bash
#
# One-time host setup so binder_demo can run (from scripts/run.sh, CLion, a
# debugger, or the shell).
#
#   sudo scripts/setup-binder-host.sh [--build-dir DIR]
#
# Loads binder_linux, mounts binderfs, creates the binder devices and makes them
# world-accessible so your normal user can open them -- the same 0666 mode
# Android uses for /dev/binder.
#
# This does not survive a reboot; re-run it afterwards. To undo:
#   sudo scripts/setup-binder-host.sh --teardown

set -euo pipefail

BINDERFS_MNT=/dev/binderfs
DEVICES=(binder hwbinder vndbinder)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log() { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[setup]\033[0m %s\n' "$*" >&2; }

BUILD_DIR=""
TEARDOWN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --teardown)  TEARDOWN=1; shift ;;
        --build-dir) BUILD_DIR=${2:-}; shift 2 ;;
        *)           err "unknown argument: $1"; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    err "must run as root:  sudo $0"
    exit 1
fi

# --------------------------------------------------------------------------
if [ "$TEARDOWN" = 1 ]; then
    for d in "${DEVICES[@]}"; do rm -f "/dev/$d"; done
    if mountpoint -q "$BINDERFS_MNT"; then
        umount "$BINDERFS_MNT"
        log "unmounted $BINDERFS_MNT"
    fi
    rmdir "$BINDERFS_MNT" 2>/dev/null || true
    log "teardown complete (binder_linux left loaded)"
    exit 0
fi

# --------------------------------------------------------------------------
# binderfs_ctl is built by the project; find it in either build layout.
CTL=""
for candidate in \
    ${BUILD_DIR:+"$BUILD_DIR/binderfs_ctl"} \
    "$REPO_ROOT/build/binderfs_ctl" \
    "$REPO_ROOT/cmake-build-debug/binderfs_ctl" \
    "$REPO_ROOT/cmake-build-release/binderfs_ctl"; do
    if [ -x "$candidate" ]; then CTL="$candidate"; break; fi
done

if [ -z "$CTL" ]; then
    err "binderfs_ctl not found -- build the project first, e.g."
    err "  cmake --build build --target binderfs_ctl"
    err "or just use scripts/run.sh, which builds it for you."
    exit 1
fi
log "using $CTL"

# 1. driver
if ! grep -qw binder /proc/filesystems 2>/dev/null; then
    log "loading binder_linux"
    modprobe binder_linux || {
        err "modprobe binder_linux failed -- does this kernel have CONFIG_ANDROID_BINDER_IPC?"
        err "  grep ANDROID_BINDER /boot/config-\$(uname -r)"
        exit 1
    }
fi

# 2. binderfs
if ! mountpoint -q "$BINDERFS_MNT"; then
    log "mounting binderfs at $BINDERFS_MNT"
    mkdir -p "$BINDERFS_MNT"
    mount -t binder binder "$BINDERFS_MNT"
fi

# 3. devices (CONFIG_ANDROID_BINDER_DEVICES is empty on distro kernels)
log "creating binder devices"
"$CTL" "$BINDERFS_MNT" "${DEVICES[@]}"

# 4. symlinks + permissions
#
# binderfs creates nodes as root:root 0600. Android ships /dev/binder as 0666;
# matching that lets your desktop user (and therefore CLion) open the driver
# without running the demo as root.
for d in "${DEVICES[@]}"; do
    ln -sf "$BINDERFS_MNT/$d" "/dev/$d"
    chmod 0666 "$BINDERFS_MNT/$d"
done

log "ready:"
ls -lL /dev/binder /dev/hwbinder /dev/vndbinder | sed 's/^/         /'
echo
log "you can now run:  $(dirname "$CTL")/binder_demo"
