#!/bin/bash
# Fills the ogles2-vsync-2 package directory ($1): the vsync report
# ("aglSwapBuffers with OGLES3_CCT_VSYNC sometimes waits two vblanks"):
# vsync_probe (no GL: WaitBOVP, WaitTOF and the HVS's frame counter timed
# against the HVS) and the reporter's VsyncTest. Called by ../build_package.sh.
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
"${CC[@]}" "$REPO/tests/vsync/vsync_probe.c" -o "$PKG/bin/vsync_probe"
"${CC[@]}" -DUSE_OGLES2 "$REPO/tests/vsync/VsyncTest.c" -L"$B" -logles2 -o "$PKG/bin/VsyncTest"
