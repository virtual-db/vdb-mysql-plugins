#!/usr/bin/env bash
# dev.sh: local development builds in a persistent container. The source tree
# and build directory live in a Docker volume, so a build that runs out of
# memory resumes where it stopped instead of starting over.
#
#   build/dev.sh server   fetch, configure and build mysqld (resumable)
#   build/dev.sh plugin   rebuild vdb.so from src/ only
#   build/dev.sh shell    a shell in the build container
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=vdb-mysql-plugins:toolchain
VOL=vdb-mysql-work
JOBS="${JOBS:-2}"
docker build -q --target toolchain -t "$IMAGE" . >/dev/null
docker volume create "$VOL" >/dev/null
run() {
  docker run --rm -v "$VOL":/work -v "$HOME/.cache/vdb":/cache -v "$PWD/build":/vdb/build:ro \
    -v "$PWD/src":/vdb/src:ro -e JOBS="$JOBS" "$IMAGE" bash -c "$1"
}
case "${1:-server}" in
server)
  run 'set -e
    [ -f /work/mysql/CMakeLists.txt ] || /vdb/build/build.sh fetch
    [ -f /work/build/CMakeCache.txt ] || /vdb/build/build.sh configure
    for i in 1 2 3 4 5 6; do cmake --build /work/build -j "$JOBS" && exit 0; echo "build attempt $i failed; resuming"; done
    exit 1' ;;
plugin)
  run '/vdb/build/build.sh plugin' ;;
shell)
  docker run --rm -it -v "$VOL":/work -v "$PWD/src":/vdb/src:ro "$IMAGE" bash ;;
esac
