#!/bin/bash
# The Pi 4's output must not change while the VC4 driver is added to
# v3dglslc (PI3_PLAN §4.1, P4): compiles every shader pair at hand with the
# host v3dglslc (build_host.sh) and saves the .v3dp/.v3dw files as the
# baseline, or compares them byte for byte with it.
#   usage: v42_unchanged.sh save     (before a change: the baseline)
#          v42_unchanged.sh check    (after it: every file must be identical)
# The pairs: tools/v3dglslc/test/shaders (as compare_v3dp.sh), the corpus'
# programs ($B/corpus/src, tests/corpus/build_corpus.sh), the release's
# examples ($RW/src, release/build_release.sh) and Boing 2026's shaders.
set -u
MODE=${1:-check}
OUT=${OUT:-$HOME/ogles2-build/v3dglslc}
B=${B:-$HOME/ogles2-build/m68k}
RW=${RW:-$HOME/ogles2-build/release/work}
T="$(dirname "$(readlink -f "$0")")"
REPO="$(dirname "$(dirname "$(dirname "$T")")")"
V3DGLSLC=$OUT/host/v3dglslc
W=$OUT/v42
BASE=$OUT/v42-baseline
rm -rf "$W" && mkdir -p "$W"

compile() { # set dir pairs...
    local set=$1 dir=$2
    shift 2
    mkdir -p "$W/$set"
    (cd "$dir" && "$V3DGLSLC" -o "$W/$set" "$@" > "$W/$set.log" 2>&1) || echo "$set: v3dglslc failed ($W/$set.log)"
}

S=$W/src-test && mkdir -p "$S" && cp "$T"/shaders/*.vert "$T"/shaders/*.frag "$S/" && sed -i 's/\r$//' "$S"/*
compile test "$S" tex.vert tex.frag light.vert light.frag skin.vert skin.frag tex.vert proc.frag \
    state.vert state.frag mrt.vert mrt.frag
if [ -d "$B/corpus/src" ]; then
    P=$(cd "$B/corpus/src" && for v in *.vert; do echo "$v ${v%.vert}.frag"; done)
    compile corpus "$B/corpus/src" $P
else
    echo "no corpus sources in $B/corpus/src (tests/corpus/build_corpus.sh)"
fi
if [ -d "$RW/src" ]; then
    compile examples "$RW/src" triangle.vert triangle.frag cube.vert cube.frag boingocean.vert boingocean.frag \
        blit.vert blit.frag height.vert height.frag sky.vert sky.frag
else
    echo "no example sources in $RW/src (release/build_release.sh)"
fi
S=$W/src-b26 && mkdir -p "$S" && cp "$REPO"/release/examples/boing2026/shaders/* "$S/" && sed -i 's/\r$//' "$S"/*
compile boing2026 "$S" ball.vert ball.frag room.vert room.frag screen.vert blur.frag screen.vert composite.frag \
    shadow.vert shadow.frag flare.vert flare.frag

N=$(find "$W" -name '*.v3d[pw]' | wc -l)
case $MODE in
save)
    rm -rf "$BASE" && mkdir -p "$BASE"
    for d in test corpus examples boing2026; do [ -d "$W/$d" ] && cp -r "$W/$d" "$BASE/"; done
    echo "baseline saved: $N files in $BASE"
    ;;
check)
    [ -d "$BASE" ] || { echo "no baseline in $BASE (run: $0 save)"; exit 1; }
    same=0 diff=0
    for f in $(cd "$BASE" && find . -name '*.v3d[pw]' | sort); do
        if cmp -s "$BASE/$f" "$W/$f"; then
            same=$((same + 1))
        else
            diff=$((diff + 1))
            echo "DIFFERENT: $f"
        fi
    done
    extra=$(cd "$W" && find . -name '*.v3d[pw]' | while read -r f; do [ -e "$BASE/$f" ] || echo "$f"; done)
    [ -n "$extra" ] && echo "NEW (not in the baseline): $extra"
    echo "V3D 4.2 output: $same identical, $diff different, of $(cd "$BASE" && find . -name '*.v3d[pw]' | wc -l)"
    [ $diff -eq 0 ] && [ -z "$extra" ]
    ;;
*)
    echo "usage: $0 save|check"
    exit 2
    ;;
esac
