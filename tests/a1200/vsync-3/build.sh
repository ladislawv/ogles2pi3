#!/bin/bash
# Fills the ogles2-vsync-3 package directory ($1): m6_vsync (the white bar,
# plan 7.2) to watch for tearing with the swap's wait on the HVS's frame
# counter, and with the ogles2.library in LIBS: for comparison. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
make -C "$REPO" -j8 B="$B" "$B/ogles2.library" "$B/tests/m6_vsync" > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m6_vsync" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$PKG/bin/"
