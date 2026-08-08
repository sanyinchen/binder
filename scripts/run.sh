#!/usr/bin/env bash
#
# Build the image if needed, then run it with the privileges binderfs requires.
#
#   scripts/run.sh                 # end-to-end demo
#   scripts/run.sh shell           # interactive shell inside the container
#   scripts/run.sh servicemanager  # just servicemanager, in the foreground
#
# --privileged + /lib/modules are needed to load binder_linux and mount
# binderfs. The driver lives in the host kernel; containers share it.

set -euo pipefail

IMAGE=${IMAGE:-android-binder}
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Some Docker Desktop installs leave a credential helper in ~/.docker/config.json
# that is not on PATH; an empty config avoids that without touching the user's.
DOCKER_CFG="$(mktemp -d)"
echo '{}' > "$DOCKER_CFG/config.json"
trap 'rm -rf "$DOCKER_CFG"' EXIT
export DOCKER_CONFIG="$DOCKER_CFG"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> building $IMAGE"
    docker build -f "$REPO_ROOT/docker/Dockerfile" -t "$IMAGE" "$REPO_ROOT"
fi

# -it only when there actually is a terminal, so this works in CI too.
TTY_FLAGS=()
if [ -t 0 ] && [ -t 1 ]; then
    TTY_FLAGS=(-it)
fi

# --init puts tini at PID 1.
#
# Without it the foreground process (servicemanager, demo_service, ...) becomes
# PID 1 itself, and the kernel discards signals whose disposition is still the
# default -- so Ctrl+C does nothing and the only way out is `docker kill` from
# another terminal. tini installs handlers and forwards them, so Ctrl+C works
# and orphaned children get reaped.
exec docker run --rm --init "${TTY_FLAGS[@]}" \
    --privileged \
    -v /lib/modules:/lib/modules:ro \
    "$IMAGE" "$@"
