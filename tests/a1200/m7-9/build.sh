#!/bin/bash
# Fills the ogles2-m7-9 package directory ($1): the disk cache's size (m7_cache,
# ogles2/compiler.c): the first compile of a library's load removes the oldest of
# its files over ENV:OGLES2/CacheKB, and nothing else. It needs a compiler: the on-target
# one in libs/ (p5_run.sh replaces it with a stand-in and compiles on the PC). The
# sync-logging library. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k-sync}
V=${V:-$HOME/ogles2-build-pi3/v3dglslc}
make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B/tests/m7_cache" "$PKG/bin/"
