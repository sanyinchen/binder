#!/usr/bin/env bash
#
# Build the project on this machine, make sure the binder driver is up, then
# run the demo.
#
#   scripts/run.sh                 # end-to-end demo
#   scripts/run.sh servicemanager  # just servicemanager, in the foreground
#   scripts/run.sh service         # just the demo service, in the foreground
#   scripts/run.sh client          # just the demo client, once
#
# The binder driver is a host kernel component (CONFIG_ANDROID_BINDER_IPC +
# CONFIG_ANDROID_BINDERFS). If /dev/binder is missing this calls
# scripts/setup-binder-host.sh through sudo to load the module, mount binderfs
# and create the devices; that part needs root, the demo itself does not.
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
    binder_demo servicemanager demo_service demo_client binderfs_ctl

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
# binder_demo finds its companion binaries next to itself, so every mode goes
# through it -- no separate paths to keep in sync.
# --------------------------------------------------------------------------
exec "$BUILD_DIR/binder_demo" "$@"
