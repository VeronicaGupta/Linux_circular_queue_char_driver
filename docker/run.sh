#!/usr/bin/env bash
set -euo pipefail

COMMAND="${1:-test}"
IMAGE_NAME="linux-character-driver-lab:ubuntu24"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$COMMAND" in
    clean)
        find "$PROJECT_ROOT/docker/output" -mindepth 1 ! -name .gitkeep -delete 2>/dev/null || true
        exit 0
        ;;
    build|test|shell|image)
        ;;
    *)
        echo "usage: $0 {build|test|shell|clean|image}" >&2
        exit 2
        ;;
esac

docker build --platform linux/amd64 -t "$IMAGE_NAME" \
    -f "$PROJECT_ROOT/docker/Dockerfile" "$PROJECT_ROOT"

[[ "$COMMAND" == image ]] && exit 0

TTY=()
[[ "$COMMAND" == shell ]] && TTY=(-it)

docker run --rm --platform linux/amd64 "${TTY[@]}" \
    --mount "type=bind,source=$PROJECT_ROOT,target=/src" \
    -w /src "$IMAGE_NAME" /src/docker/lab.sh "$COMMAND"
