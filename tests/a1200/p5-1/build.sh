#!/bin/bash
# Fills the ogles2-p5-1 package directory ($1): the Pi 3 card's first run
# (docs/PI3_PLAN.md §7, P5). vc4core's cases built for AmigaOS (the Makefile's
# tests/vc4_draw) with the .vc4p programs they draw and Mesa's pictures from
# the test Pi 3 (its reference boot, tests/pi3/ref/out), then the
# sync-logging library's first clears through the VC4 backend. Called by
# ../build_package.sh (WSL; the Pi 3 reachable as "pi3" for Mesa's pictures).
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build-pi3/m68k}
BS=${BS:-$HOME/ogles2-build-pi3/m68k-sync}
P3=${P3:-$HOME/ogles2-build-pi3/pi3}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" "$B/tests/vc4_draw" > /dev/null
bash "$REPO/tests/pi3/vc4core/build.sh" > /dev/null # the .vc4p files the cases draw
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/vc4p" "$PKG/ref"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$B/tests/vc4_draw" "$BS/tests/m1_clear" "$BS/tests/smoke" "$BS/cmd/OGLES2Log" "$PKG/bin/"
cp "$P3"/vc4p/*.vc4p "$PKG/vc4p/"
# REF=<dir>: Mesa's pictures from there (a copy kept while the Pi 3 is in the PiStorm)
if [ -n "${REF:-}" ]; then
    cp "$REF"/mesa_*.ppm "$PKG/ref/"
else
    scp -q 'pi3:pi3/ref/out/mesa_*.ppm' "$PKG/ref/" || { echo "Mesa's pictures: the Pi 3 (pi3) did not answer (REF=<dir> takes a copy)"; exit 1; }
fi
