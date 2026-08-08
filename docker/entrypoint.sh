#!/bin/bash
#
# Container entrypoint: brings up the binder driver, then dispatches a command.
#
#   demo             servicemanager + demo_service + demo_client, then exit
#   servicemanager   run servicemanager in the foreground
#   service          run the demo service in the foreground
#   client           run the demo client once
#   shell            interactive shell with the environment set up
#   <anything else>  executed as-is
#
# Requires --privileged (or CAP_SYS_ADMIN + CAP_SYS_MODULE) so the container can
# load binder_linux and mount binderfs. The binder driver itself lives in the
# host kernel -- containers share it, so there is nothing to compile here.

set -euo pipefail

BINDERFS_MNT=/dev/binderfs
BIN_DIR=${BIN_DIR:-/usr/local/bin}
LOG_DIR=${LOG_DIR:-/var/log/binder}

log() { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[setup]\033[0m %s\n' "$*" >&2; }

# --------------------------------------------------------------------------
# 1. binder driver
# --------------------------------------------------------------------------
setup_binder() {
    if [ -e /dev/binder ]; then
        log "/dev/binder already present"
        return 0
    fi

    if ! grep -qw binder /proc/filesystems 2>/dev/null; then
        log "binderfs not registered, loading binder_linux"
        if ! modprobe binder_linux 2>/dev/null; then
            err "modprobe binder_linux failed."
            err "Run the container with --privileged -v /lib/modules:/lib/modules:ro,"
            err "or load the module on the host first: sudo modprobe binder_linux"
            exit 1
        fi
    fi

    if ! mountpoint -q "$BINDERFS_MNT" 2>/dev/null; then
        log "mounting binderfs at $BINDERFS_MNT"
        mkdir -p "$BINDERFS_MNT"
        if ! mount -t binder binder "$BINDERFS_MNT"; then
            err "mount -t binder failed -- the container needs --privileged."
            exit 1
        fi
    fi

    # CONFIG_ANDROID_BINDER_DEVICES is empty on most distro kernels, so the
    # devices have to be created explicitly through binder-control.
    log "creating binder devices"
    "$BIN_DIR/binderfs_ctl" "$BINDERFS_MNT" binder hwbinder vndbinder

    ln -sf "$BINDERFS_MNT/binder" /dev/binder
    ln -sf "$BINDERFS_MNT/hwbinder" /dev/hwbinder
    ln -sf "$BINDERFS_MNT/vndbinder" /dev/vndbinder

    log "binder ready: $(ls -l /dev/binder)"
}

start_service_manager() {
    mkdir -p "$LOG_DIR"
    log "starting servicemanager"
    "$BIN_DIR/servicemanager" >"$LOG_DIR/servicemanager.log" 2>&1 &
    SM_PID=$!
    sleep 0.5
    if ! kill -0 "$SM_PID" 2>/dev/null; then
        err "servicemanager exited immediately:"
        cat "$LOG_DIR/servicemanager.log" >&2
        exit 1
    fi
    log "servicemanager running (pid $SM_PID)"
}

# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
setup_binder

CMD=${1:-demo}
shift || true

case "$CMD" in
    demo)
        # main.cpp drives the demo: it spawns servicemanager and demo_service,
        # then runs demo_client against them.
        exec "$BIN_DIR/binder_demo" demo
        ;;

    servicemanager)
        log "running servicemanager in foreground"
        exec "$BIN_DIR/servicemanager" "$@"
        ;;

    service)
        start_service_manager
        exec "$BIN_DIR/demo_service" "$@"
        ;;

    client)
        exec "$BIN_DIR/demo_client" "$@"
        ;;

    shell)
        exec /bin/bash "$@"
        ;;

    *)
        exec "$CMD" "$@"
        ;;
esac
