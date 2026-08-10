#!/usr/bin/env bash
#
# Build the project on this machine, make sure the binder driver is up, then
# bring the services up.
#
#   scripts/run.sh                      # servicemanager + http + download services
#   scripts/run.sh servicemanager       # just servicemanager, in the foreground
#   scripts/run.sh http                 # just the http service, in the foreground
#   scripts/run.sh download             # just the download service, in the foreground
#   scripts/run.sh client <url> [path]  # run the download client once
#
# The services are long-lived; Ctrl+C stops them. The client wants a second
# terminal:
#
#   terminal A: scripts/run.sh
#   terminal B: scripts/run.sh client https://example.com/file.bin
#
# The binder driver is a host kernel component (CONFIG_ANDROID_BINDER_IPC +
# CONFIG_ANDROID_BINDERFS). If /dev/binder is missing this calls
# scripts/setup-binder-host.sh through sudo to load the module, mount binderfs
# and create the devices; that part needs root, the services do not.
#
# Environment:
#   BUILD_DIR   build tree to use   (default: <repo>/build)
#   CC / CXX    compilers           (default: auto-detected clang)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR=${BUILD_DIR:-$REPO_ROOT/build}

log() { printf '\033[1;34m[run]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[run]\033[0m %s\n' "$*" >&2; }

# --------------------------------------------------------------------------
# 1. build
#
# The AOSP sources under external/android/ do not compile with GCC, so clang is
# pinned here even when it is not the system compiler. Distros commonly ship
# only versioned binaries (clang-18, no plain clang), and a side-by-side install
# of two versions can leave one of them unable to run at all:
#
#   CommandLine Error: Option '...' registered more than once!
#
# So candidates are probed by actually compiling something, newest first, and
# the first pair that works wins.
# --------------------------------------------------------------------------
works() { echo 'int main(){return 0;}' | "$1" -x c -fsyntax-only - >/dev/null 2>&1; }

find_clang() {
    local suffix
    for suffix in "" $(seq 20 -1 14 | sed 's/^/-/'); do
        if command -v "clang$suffix" >/dev/null 2>&1 &&
           command -v "clang++$suffix" >/dev/null 2>&1 &&
           works "clang$suffix" && works "clang++$suffix"; then
            echo "$suffix"
            return 0
        fi
    done
    return 1
}

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ]; then
        if ! SUFFIX=$(find_clang); then
            err "no working clang found -- install it first:"
            err "  sudo apt update && sudo apt install --no-install-recommends clang"
            err "(or point CC/CXX at one yourself)"
            exit 1
        fi
        CC=${CC:-clang$SUFFIX}
        CXX=${CXX:-clang++$SUFFIX}
    fi

    GENERATOR=()
    command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
    log "configuring $BUILD_DIR with $CXX"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" "${GENERATOR[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX"
fi

log "building"
cmake --build "$BUILD_DIR" -j "$(nproc)" --target \
    service_manager download_client servicemanager binderfs_ctl

# --------------------------------------------------------------------------
# 2. binder driver
# --------------------------------------------------------------------------
if [ ! -e /dev/binder ]; then
    log "/dev/binder missing, running scripts/setup-binder-host.sh (needs root)"
    sudo "$REPO_ROOT/scripts/setup-binder-host.sh" --build-dir "$BUILD_DIR"
fi

# --------------------------------------------------------------------------
# 3. run
#
# Every mode but the client goes through service_manager, so there is no second
# set of paths to keep in sync.
# --------------------------------------------------------------------------
MODE=${1:-all}
shift || true

case "$MODE" in
    client)
        exec "$BUILD_DIR/download_client" "$@"
        ;;
    all|http|download|servicemanager)
        exec "$BUILD_DIR/service_manager" "$MODE" "$@"
        ;;
    *)
        err "unknown mode: $MODE"
        err "usage: scripts/run.sh [all|http|download|servicemanager|client <url> [path]]"
        exit 2
        ;;
esac
