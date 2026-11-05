#!/bin/bash
# Fills the ogles2-m7-1 package directory ($1): dEQP-GLES2 (ports/deqp) with
# its data, the sync-logging library, the on-target shader compiler (every
# test's shaders are kept in cache/) and v3dglslc-remote, which sends them to
# the PC to compile instead (tests/a1200/p2_deqp.sh starts it).
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
BS=${BS:-$HOME/ogles2-build-pi3/m68k-sync}
V=${V:-$HOME/ogles2-build-pi3/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/ports/deqp/build.sh" > /dev/null 2>&1 || { echo "ports/deqp/build.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/cache"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B/deqp/deqp-gles2" "$BS/cmd/OGLES2Log" "$B/tests/m7_points" "$B/amictl/v3dglslc-remote" "$PKG/bin/"
cp -r "$B/deqp/gles2" "$PKG/bin/gles2"
# The must-pass list, a file per group (Run-M1 LIST=<group>).
bash "$REPO/ports/deqp/caselists.sh" "$PKG/caselists" > /dev/null
