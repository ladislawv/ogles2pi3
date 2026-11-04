#!/bin/bash
# PI3_PLAN P2 (§5.1): the Pi 4 checks after each step of the backend split,
# against a baseline taken the same day. Builds the packages from this
# repository (~/ogles2-build-pi3), runs them on the A1200 through amictl (the
# Pi 4 card in, nothing in LIBS:) and prints what to compare:
#   m5-1: 255 PASS and no FAIL; m7-6: 36 of 36; m2-5: its 14 pictures byte
#   for byte against REF (default: the run of 2026-10-14); m6-1 and Quake
#   III's timedemo (package p2v-m6-5, from tests/a1200/p2_vc4keys.sh): fps.
# Run from WSL or Git Bash in the repository:
#   bash tests/a1200/p2_checks.sh [m5-1 m7-6 m2-5 m6-1 q3]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
REF=${REF:-$RES/ogles2-m2-5-20261014-175819}
WANT=${*:-m5-1 m7-6 m2-5 m6-1 q3}
case "$(uname -s)" in
    MINGW* | MSYS*) PY=python; WSL=1 ;;
    *) PY=python3; WSL=0 ;;
esac
amictl() { "$PY" "$REPO/tools/amictl/amictl.py" "$@"; }
# The repository as WSL sees it (Git Bash: /c/... -> /mnt/c/...).
WREPO=$(echo "$REPO" | sed -E 's#^/([a-z])/#/mnt/\1/#')

build() { # package
    if [ $WSL = 1 ]; then
        MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 bash -c "cd '$WREPO' && bash tests/a1200/build_package.sh $1" > /dev/null 2>&1
    else
        (cd "$REPO" && bash tests/a1200/build_package.sh "$1") > /dev/null 2>&1
    fi
}
newest() { ls -d "$RES"/$1-* 2>/dev/null | tail -1; }

for t in $WANT; do
    case $t in
    q3)
        lib=$HOME/ogles2-build-pi3/m68k/ogles2.library
        [ $WSL = 1 ] && lib=$(MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 wslpath -w /home/l/ogles2-build-pi3/m68k/ogles2.library)
        cp "$lib" "$HERE/out/p2v-m6-5/libs/ogles2.library" || exit 1
        amictl runpkg "$HERE/out/p2v-m6-5" -t 1800 -a "FRAMES=0" > /dev/null
        echo "q3: $(grep -h 'frames.*fps' "$(newest p2v-m6-5)"/results/qconsole.log)"
        ;;
    *)
        build "$t" || { echo "$t: build failed"; continue; }
        amictl runpkg "$HERE/out/ogles2-$t" -t 1200 > /dev/null
        R=$(newest "ogles2-$t")/results
        case $t in
        m5-1 | m7-6) echo "$t: $(grep -h '^PASS' "$R"/*.txt | wc -l) PASS, $(grep -h '^FAIL' "$R"/*.txt | wc -l) FAIL" ;;
        m2-5)
            same=0 diff=0
            for f in "$R"/*.ppm; do
                if cmp -s "$f" "$REF/results/$(basename "$f")"; then same=$((same + 1)); else diff=$((diff + 1)); fi
            done
            echo "m2-5: $same pictures identical, $diff different (against $(basename "$REF"))" ;;
        m6-1) echo "m6-1:"; grep -h "^RESULT" "$R"/*.txt | sed 's/^/  /' ;;
        esac
        ;;
    esac
done
