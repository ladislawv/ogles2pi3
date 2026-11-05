#!/bin/bash
# The VC4 backend's S3TC decoder against Mesa's, on the PC (s3tctest.c).
#   bash tests/pi3/s3tc/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3}
M=~/mesa-build/mesa/build
mkdir -p "$OUT"
gcc -O2 -Wall -Wextra -std=gnu99 "-I$REPO/ogles2/vc4" -o "$OUT/s3tctest" "$HERE/s3tctest.c" "$REPO/ogles2/vc4/s3tc.c" \
    "$M/src/util/libmesa_util.a" -lm -lpthread -lz -lzstd 2>/dev/null ||
    gcc -O2 -Wall -Wextra -std=gnu99 "-I$REPO/ogles2/vc4" -o "$OUT/s3tctest" "$HERE/s3tctest.c" "$REPO/ogles2/vc4/s3tc.c" \
        "$M/src/util/libmesa_util.a" -lm -lpthread
"$OUT/s3tctest"
