#!/bin/bash
# Fills the ogles2-vsync-4 package directory ($1): vsync_watch (skipped
# refreshes, doubled and late frames, judder, from the HVS) with the
# ogles2.library in LIBS: and with this package's, alternately. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
make -C "$REPO" -j8 B="$B" "$B/ogles2.library" "$B/libogles2.a" > /dev/null
TC=${TC:-$HOME/amiga-gcc-install}
CC=("$TC/bin/m68k-amigaos-gcc" -std=gnu99 -O2 -Wall -mcpu=68020 -m68881 -mcrt=clib2 "-I$REPO/include" "-I$B/gen/include")
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles2.library" "$PKG/libs/"
"${CC[@]}" "$REPO/tests/vsync/vsync_watch.c" -L"$B" -logles2 -o "$PKG/bin/vsync_watch"
