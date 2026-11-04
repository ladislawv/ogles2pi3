#!/bin/bash
# PI3_PLAN P2 (§4.2): the probe-V packages. Takes built packages (from the
# ogles2 checkout's tests/a1200/out, or $SRC), swaps in this repository's
# ogles2.library (built into $B: this repository's ~/ogles2-build-pi3/m68k,
# never the ogles2 checkout's ~/ogles2-build) and the run scripts with the
# PROBE= argument, and
# writes them as tests/a1200/out/p2v-<name>. Run in WSL:
#   bash tests/a1200/p2_vc4keys.sh
# Then, with the Pi 4 card in the A1200 (amictl from Windows or WSL):
#   amictl setenv OGLES2/Probe V
#   amictl runpkg tests/a1200/out/p2v-m2-5 -t 900      (the corpus)
#   amictl runpkg tests/a1200/out/p2v-m6-3 -t 900      (SDL2's tests)
#   amictl runpkg tests/a1200/out/p2v-m6-4 -t 900      (SDL3's tests)
#   amictl unsetenv OGLES2/Probe
#   amictl runpkg tests/a1200/out/p2v-m6-5 -t 1800 -a "PROBE=TPV FRAMES=0"  (Quake III, renderergl2)
#   amictl runpkg tests/a1200/out/p2v-m6-2 -t 1800 -a "PROBE=TPV FRAMES=0"  (Quake III on gl4es)
# and summarise each results/ogles2.log with tests/a1200/vc4keys.py.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SRC=${SRC:-"/mnt/c/- CPP/ogles2/tests/a1200/out"}
B=${B:-$HOME/ogles2-build-pi3/m68k}
OUT=$HERE/out

make -C "$REPO" -j16 B="$B" "$B/ogles2.library" > /dev/null
for name in m2-5 m6-2 m6-3 m6-4 m6-5; do
    [ -d "$SRC/ogles2-$name" ] || { echo "no $SRC/ogles2-$name (build it in the ogles2 checkout)"; exit 1; }
    d=$OUT/p2v-$name
    rm -rf "$d"
    mkdir -p "$OUT"
    cp -r "$SRC/ogles2-$name" "$d"
    rm -rf "$d/results"
    cp "$B/ogles2.library" "$d/libs/ogles2.library"
    if [ -f "$HERE/$name/Run-M1" ]; then
        sed 's/\r$//' "$HERE/$name/Run-M1" > "$d/Run-M1"
    fi
    echo "$d: $(ls "$d" | tr '\n' ' ')"
done
