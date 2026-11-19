#!/bin/bash
# Fills the ogles2-upload package directory ($1): the 16-bit upload report's
# test (tests/perf/upload_test.c, its USE_OGLES2 build; docs/PI3_PLAN.md,
# "Plan: ogles3's 16-bit upload fixes"). p5_run.sh puts the sync-logging
# library in libs/; a speed run gives LIB=<the plain build's>. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
TC=${TC:-$HOME/amiga-gcc-install}

make -C "$REPO" -j8 B="$B" "$B/ogles2.library" "$B/libogles2.a" > /dev/null
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles2.library" "$PKG/libs/"
"$TC/bin/m68k-amigaos-gcc" -std=gnu99 -O2 -mcpu=68020 -m68881 -mcrt=clib2 -Wall -DUSE_OGLES2 -I"$REPO/include" \
    -I"$B/gen/include" "$REPO/tests/perf/upload_test.c" -L"$B" -logles2 -o "$PKG/bin/upload_test"
