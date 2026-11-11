#!/bin/bash
# Fills the ogles2-m8-1 package directory ($1): a framebuffer object's job
# queued behind the running render (job.c), m8_queue on a screen of its own,
# then the same with ENV:OGLES2/NoQueue as the control. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m8_queue" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$PKG/bin/"
