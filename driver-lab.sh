#!/usr/bin/env bash
set -euo pipefail
IMAGE=char-driver-lab
COMMAND="${1:-help}"

case "$COMMAND" in
    image)
        docker build -t "$IMAGE" -f docker/Dockerfile .
        ;;
    build|test|shell|clean|help)
        mkdir -p docker/output
        if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
            docker build -t "$IMAGE" -f docker/Dockerfile .
        fi
        if [[ "$COMMAND" == "shell" ]]; then
            docker run --rm -it \
                -v "$(pwd)/docker/output:/workspace/docker/output" \
                "$IMAGE" shell
        else
            docker run --rm \
                -v "$(pwd)/docker/output:/workspace/docker/output" \
                "$IMAGE" "$COMMAND"
        fi
        ;;
    *)
        echo "Usage: $0 <image|build|test|shell|clean|help>" >&2
        exit 2
        ;;
esac
