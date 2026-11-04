#!/bin/bash
# Compiles every shader pair v42_unchanged.sh uses for the Raspberry Pi 3's
# VC4 (v3dglslc -g 21 -o, docs/PI3_PLAN.md P4) and reports per program:
# written (with its variants) or failed, and why (the driver's message).
#   usage: vc4_corpus.sh      -> $OUT/vc4corpus/<set>/*.vc4p, *.log
set -u
OUT=${OUT:-$HOME/ogles2-build/v3dglslc}
B=${B:-$HOME/ogles2-build/m68k}
RW=${RW:-$HOME/ogles2-build/release/work}
T="$(dirname "$(readlink -f "$0")")"
REPO="$(dirname "$(dirname "$(dirname "$T")")")"
V3DGLSLC=$OUT/host/v3dglslc
W=$OUT/vc4corpus
rm -rf "$W" && mkdir -p "$W"
ok=0 bad=0

one() { # set dir vs fs
    local set=$1 dir=$2 vs=$3 fs=$4 log
    mkdir -p "$W/$set"
    log=$W/$set/${vs%.*}-${fs%.*}.log
    (cd "$dir" && "$V3DGLSLC" -g 21 -o "$W/$set" "$vs" "$fs" > "$log" 2>&1)
    if [ -f "$W/$set/${vs%.*}-${fs%.*}.vc4p" ]; then
        ok=$((ok + 1))
        printf "  %-10s %-34s %s\n" "$set" "${vs%.*}-${fs%.*}" "$(grep -c '^vc4-variant' "$log") variants"
    else
        bad=$((bad + 1))
        printf "  %-10s %-34s FAILED: %s\n" "$set" "${vs%.*}-${fs%.*}" \
            "$(grep -v '^vc4-variant\|^    0x\|^  ' "$log" | grep -iE 'fail|error|abort|assert' | head -2 | tr '\n' ' ')"
    fi
}

S=$W/src-test && mkdir -p "$S" && cp "$T"/shaders/*.vert "$T"/shaders/*.frag "$S/" && sed -i 's/\r$//' "$S"/*
for p in "tex tex" "light light" "skin skin" "tex proc" "state state" "mrt mrt"; do
    set -- $p
    one test "$S" "$1.vert" "$2.frag"
done
if [ -d "$B/corpus/src" ]; then
    for v in "$B"/corpus/src/*.vert; do one corpus "$B/corpus/src" "$(basename "$v")" "$(basename "${v%.vert}").frag"; done
fi
if [ -d "$RW/src" ]; then
    for p in triangle cube boingocean blit height sky; do one examples "$RW/src" "$p.vert" "$p.frag"; done
fi
S=$W/src-b26 && mkdir -p "$S" && cp "$REPO"/release/examples/boing2026/shaders/* "$S/" && sed -i 's/\r$//' "$S"/*
for p in "ball ball" "room room" "screen blur" "screen composite" "shadow shadow" "flare flare"; do
    set -- $p
    one boing2026 "$S" "$1.vert" "$2.frag"
done
echo "VC4: $ok programs written, $bad failed ($W)"
