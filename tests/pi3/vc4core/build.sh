#!/bin/bash
# vc4core's Linux tests (docs/PI3_PLAN.md §3.1, P3), cross-built in WSL for
# the Pi 3 (aarch64, static) into ~/ogles2-build-pi3/pi3; with "m68k" also
# vc4core alone for the 68k (a compile check until os_amiga.c, P5).
#   bash tests/pi3/vc4core/build.sh [m68k]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3}
CORE=("$REPO/vc4core/device.c" "$REPO/vc4core/mem.c" "$REPO/vc4core/cl.c" "$REPO/vc4core/job.c" "$REPO/vc4core/tiling.c")
FLAGS=(-O2 -Wall -Wextra -std=gnu99 "-I$REPO/vc4core/include" "-I$REPO/vc4core")
mkdir -p "$OUT"
for t in triangle texture draw; do
    EXTRA=()
    [ $t = draw ] && EXTRA=("$HERE/vc4p.c")
    # -fno-store-merging: Linux maps GPU memory as device memory, where the
    # wider (and possibly unaligned) stores gcc makes of adjacent byte stores
    # fault (vc4core.h: aligned 1-, 2- and 4-byte stores only); so do the
    # vector stores of byte runs the vectoriser makes.
    aarch64-linux-gnu-gcc-14 "${FLAGS[@]}" -fno-store-merging -fno-tree-vectorize -fno-tree-slp-vectorize -static -o "$OUT/vc4_$t" "$HERE/$t.c" "${EXTRA[@]}" "${CORE[@]}" "$REPO/vc4core/os_linux.c"
    echo "built: $OUT/vc4_$t"
done
# The programs draw.c loads, compiled on the PC (S1) as the Pi 3's.
V=${V:-$HOME/ogles2-build-pi3/v3dglslc/host/v3dglslc}
mkdir -p "$OUT/vc4p"
(cd "$REPO/tests/pi3/ref/shaders" &&
"$V" -g 21 -o "$OUT/vc4p" triangle.vert triangle.frag > /dev/null 2>&1 || echo "v3dglslc failed on triangle"
# the cases of ../ref/cases.h: their states too (-s), the second input at
# location 1 as mesa_cases.c binds it (-b)
"$V" -g 21 -b pos,uv -s tex565,texlum,tex4444,tex5551,texla,etc1 -o "$OUT/vc4p" texture.vert texture.frag > /dev/null 2>&1 || echo "v3dglslc failed on texture"
"$V" -g 21 -s depth,blend,lines,stencil,rt565 -o "$OUT/vc4p" flat.vert flat.frag > /dev/null 2>&1 || echo "v3dglslc failed on flat"
"$V" -g 21 -s points -o "$OUT/vc4p" point.vert flat.frag > /dev/null 2>&1 || echo "v3dglslc failed on point"
"$V" -g 21 -b pos,col -s ubyte1 -o "$OUT/vc4p" color.vert color.frag > /dev/null 2>&1 || echo "v3dglslc failed on color"
"$V" -g 21 -b pos,dir -o "$OUT/vc4p" cube.vert cube.frag > /dev/null 2>&1 || echo "v3dglslc failed on cube")
# the VC4 backend's built-in clear program with its keys (tools/clear_vc4), for pclear
gcc -O2 "-I$REPO/ogles2/vc4" "-I$REPO/vc4core/include" -o "$OUT/clear_keys" "$REPO/tools/clear_vc4/keys.c" \
    "$REPO/ogles2/vc4/key_vc4.c" && "$OUT/clear_keys" "$OUT/clear.keys" > /dev/null
(cd "$REPO/tools/clear_vc4" &&
"$V" -g 21 -k "$OUT/clear.keys" -o "$OUT/vc4p" clear.vert clear.frag > /dev/null 2>&1 || echo "v3dglslc failed on clear")
echo "compiled: $(ls "$OUT/vc4p" | tr '
' ' ')"
if [ "${1:-}" = m68k ]; then
    PATH=$HOME/amiga-gcc-install/bin:$PATH
    # the library's flags (Makefile CFLAGS), os_amiga.c too
    for f in "${CORE[@]}" "$REPO/vc4core/os_amiga.c"; do
        m68k-amigaos-gcc "${FLAGS[@]}" -mcpu=68020 -m68881 -fbbb=abcfhilmnprsz0 -fno-strict-aliasing -mcrt=clib2             -c "$f" -o "$OUT/$(basename "$f" .c).m68k.o"
    done
    echo "vc4core compiles for the 68k"
fi
