#!/usr/bin/env bash
# build.sh: builds Oracle's mysqld 8.4 from source with vdb.so (implementation plan doc 14 §2.1).
#
# Usage: build.sh <step> [release]
#   fetch     download the release's source tarball, verify its SHA-256, extract to $SRC
#   configure run cmake with build/cmake.options (plus -DWITH_DEBUG=1 when VARIANT=debug)
#   server    build mysqld, the client tools and the server plug-ins (everything but vdb.so)
#   plugin    copy src/ to storage/vdb, re-run cmake, build vdb.so only
#   install   install into /usr/local/mysql and remove what VDB does not ship
#
# Environment: SRC (source tree, default /work/mysql), BLD (build dir, default /work/build),
# VDB_SRC (this repository's src/, default /vdb/src), VARIANT (release or debug), JOBS.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-/work/mysql}"
BLD="${BLD:-/work/build}"
VDB_SRC="${VDB_SRC:-/vdb/src}"
VARIANT="${VARIANT:-release}"
JOBS="${JOBS:-$(nproc)}"
CACHE="${CACHE:-/cache}"

release_line() {
  local want="${1:-}"
  grep -v '^#' "$HERE/mysql-releases.txt" | awk -v v="$want" 'NF==3 && (v=="" || $1==v) {print; exit}'
}

step="${1:?step required}"
release="${2:-}"

case "$step" in
fetch)
  read -r ver url sha < <(release_line "$release")
  [ -n "${ver:-}" ] || { echo "build.sh: release ${release:-default} not in mysql-releases.txt" >&2; exit 1; }
  mkdir -p "$CACHE" "$SRC"
  tarball="$CACHE/mysql-$ver.tar.gz"
  if [ ! -f "$tarball" ] || ! echo "$sha  $tarball" | sha256sum -c --quiet -; then
    curl -fsSL -o "$tarball.part" "$url"
    mv "$tarball.part" "$tarball"
  fi
  echo "$sha  $tarball" | sha256sum -c -
  tar xzf "$tarball" -C "$SRC" --strip-components=1
  echo "$ver" > "$SRC/.vdb-release"
  ;;
configure)
  mkdir -p "$BLD"
  mapfile -t opts < <(grep -v '^#' "$HERE/cmake.options" | sed '/^$/d')
  [ "$VARIANT" = debug ] && opts+=("-DWITH_DEBUG=1")
  cmake -S "$SRC" -B "$BLD" "${opts[@]}"
  ;;
server)
  # A parallel compile can run out of memory on small build machines; the
  # retry continues the same build directory one file at a time.
  cmake --build "$BLD" -j "$JOBS" || cmake --build "$BLD" -j 1
  ;;
plugin)
  rm -rf "$SRC/storage/vdb"
  cp -a "$VDB_SRC" "$SRC/storage/vdb"
  cmake "$BLD" >/dev/null
  cmake --build "$BLD" -j "$JOBS" --target vdb -- -k
  ;;
install)
  DESTDIR="${DESTDIR:-}" cmake --install "$BLD"
  prefix="${DESTDIR:-}/usr/local/mysql"
  rm -rf "$prefix/mysql-test" "$prefix/include" "$prefix/man" "$prefix/docs" \
         "$prefix/share/aclocal" "$prefix/lib/pkgconfig" "$prefix/run" "$prefix/var"
  find "$prefix/lib" -name '*.a' -delete
  ;;
*)
  echo "build.sh: unknown step $step" >&2; exit 1 ;;
esac
