#!/bin/bash
# vc4core's utile-at-a-time texture store against the texel-at-a-time one, on
# the PC or the Linux bench (tiling_check.c).
#   bash tests/pi3/tiling/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3}
mkdir -p "$OUT"
gcc -O2 -Wall -Wextra -std=gnu99 "-I$REPO/vc4core/include" -o "$OUT/tiling_check" "$HERE/tiling_check.c" \
    "$REPO/vc4core/tiling.c"
"$OUT/tiling_check"
