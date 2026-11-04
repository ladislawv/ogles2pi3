#!/bin/bash
# Builds the bare-GPU probe for Linux on the Raspberry Pi 3 (PI3_PLAN P1) in
# WSL, with Ubuntu's aarch64-linux-gnu GCC, statically linked:
#   bash tests/pi3_probe/build.sh     -> $OUT/pi3_probe
# Run it on the Pi as root in the bare boot (tests/pi3/boot.sh bare):
#   sudo ./pi3_probe --ref=mesa_triangle.ppm
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3}
mkdir -p "$OUT"
aarch64-linux-gnu-gcc-14 -O2 -Wall -Wextra -std=gnu99 -static -o "$OUT/pi3_probe" \
    "$HERE/pi3_probe.c" "$HERE/plat_linux.c"
echo "built: $OUT/pi3_probe"
