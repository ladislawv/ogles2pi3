#!/bin/bash
# PI3_PLAN P2 (§5.1): the Pi 4 checks after each step of the backend split,
# against a baseline taken the same day. Builds the packages from this
# repository (~/ogles2-build-pi3), runs them on the A1200 through amictl (the
# Pi 4 card in, nothing in LIBS:) and prints what to compare:
#   m5-1: 255 PASS and no FAIL (m1-5, m2-4, m6-6, m6-8, m6-10, m7-2 to m7-5 counted alike); m7-6: 36 of 36; m2-5: its 14 pictures byte
#   for byte against REF (default: the run of 2026-10-14); m6-1 and Quake
#   III's timedemo (package p2v-m6-5, from tests/a1200/p2_vc4keys.sh): fps.
# Run from WSL or Git Bash in the repository:
#   bash tests/a1200/p2_checks.sh [m5-1 m7-6 m2-5 m6-1 q3]
# OGLES2_WSL: the WSL distribution, from Git Bash (as `wsl -l` names it; default Ubuntu-24.04).
set -u
WSLD=${OGLES2_WSL:-Ubuntu-24.04}
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
        MSYS_NO_PATHCONV=1 wsl -d "$WSLD" bash -c "cd '$WREPO' && bash tests/a1200/build_package.sh $1" > /dev/null 2>&1
    else
        (cd "$REPO" && bash tests/a1200/build_package.sh "$1") > /dev/null 2>&1
    fi
}
newest() { ls -d "$RES"/$1-2[0-9]* 2>/dev/null | tail -1; } # dated: not an old "-run3", which sorts after them
# amictl's status 4 is "STATION DOWN": the rest would only report older runs (2026-11-07: the
# Amiga left the network during m5-1, and the next packages printed that morning's results)
down() { local rc=$?; [ $rc = 4 ] && { echo "$(date +%T) STATION DOWN during $1: stopped"; exit 4; }; return 0; }
WORK=
station_work() { [ -n "$WORK" ] || WORK=$(amictl run CD 2> /dev/null | tr -d '\r' | tail -1); echo "$WORK"; } # runpkg's drawer
# The package's library also in bin/libs/ (PROGDIR:libs for the programs there): OGLES2Open takes
# a copy beside the program when its version is higher than the installed one's, and the Run
# scripts' "Assign LIBS: ... ADD" comes after LIBS:'s own drawers. With an ogles2.library in
# LIBS: (the A1200's CaffeineOS:Libs has 1.3 since 2026-11-11) the packages ran that one instead
# (that day's m5-1, m7-6, m2-5, m6-1 and m8-1 runs: "1.3 init" in their logs).
own_lib() { mkdir -p "$1/bin/libs" && cp "$1/libs/ogles2.library" "$1/bin/libs/ogles2.library"; }
# Which library the run initialised (its log's first line), to see that it was the package's.
ran_lib() { grep -h ' init$' "$1"/*.log 2> /dev/null | head -1 | sed 's/^ *[0-9]* //'; }
fresh() { local r; r=$(newest "$1"); [ -n "$r" ] && ! [[ "${r##*$1-}" < "$2" ]]; } # a results drawer of this run

for t in $WANT; do
    case $t in
    q3)
        # probe V's copy if tests/a1200/p2_vc4keys.sh made one, else the plain package (built here)
        Q3=${Q3PKG:-p2v-m6-5}
        if [ ! -d "$HERE/out/$Q3" ]; then
            Q3=ogles2-m6-5
            build m6-5 || { echo "q3: build failed"; continue; }
        fi
        lib=$HOME/ogles2-build-pi3/m68k/ogles2.library
        [ $WSL = 1 ] && lib=$(MSYS_NO_PATHCONV=1 wsl -d "$WSLD" sh -c 'wslpath -w "$HOME/ogles2-build-pi3/m68k/ogles2.library"' | tr -d '\r')
        [ -n "${LIB:-}" ] && lib=$LIB
        cp "$lib" "$HERE/out/$Q3/libs/ogles2.library" || exit 1
        own_lib "$HERE/out/$Q3" || exit 1
        stamp=$(date +%Y%m%d-%H%M%S)
        amictl runpkg "$HERE/out/$Q3" -t 1800 -a "FRAMES=0" > /dev/null
        down q3
        fresh "$Q3" "$stamp" || { echo "q3: NO RESULTS from this run"; continue; }
        echo "q3: $(grep -h 'frames.*fps' "$(newest $Q3)"/results/qconsole.log) [$(ran_lib "$(newest $Q3)"/results)]"
        ;;
    *)
        build "$t" || { echo "$t: build failed"; continue; }
        # LIB=<a library>: run the package with that one (P2's acceptance: 1.3.3's)
        [ -n "${LIB:-}" ] && { cp "$LIB" "$HERE/out/ogles2-$t/libs/ogles2.library" || exit 1; }
        own_lib "$HERE/out/ogles2-$t" || exit 1
        # runpkg uploads what changed and deletes nothing: a package without a compiler of its own
        # kept the Pi 3 runs' 78-byte stand-in on the station, which made every compile wait 60 s
        # (m5-1's link step 13 minutes, 2026-11-07)
        [ -e "$HERE/out/ogles2-$t/libs/v3dglslc" ] ||
            amictl run "Delete \"$(station_work)/ogles2-$t/libs/v3dglslc\" QUIET" > /dev/null 2>&1
        stamp=$(date +%Y%m%d-%H%M%S)
        amictl runpkg "$HERE/out/ogles2-$t" -t 1200 > /dev/null
        down "$t"
        fresh "ogles2-$t" "$stamp" || { echo "$t: NO RESULTS from this run"; continue; }
        R=$(newest "ogles2-$t")/results
        echo "$t ran: $(ran_lib "$R")"
        case $t in
        m5-1 | m7-6 | m2-4 | m7-2 | m7-4 | m7-5 | m1-5 | m6-6 | m6-8 | m6-10 | m7-3 | m7-9) echo "$t: $(grep -h '^PASS' "$R"/*.txt | wc -l) PASS, $(grep -h '^FAIL' "$R"/*.txt | wc -l) FAIL" ;;
        m2-5)
            same=0 diff=0
            for f in "$R"/*.ppm; do
                if cmp -s "$f" "$REF/results/$(basename "$f")"; then same=$((same + 1)); else diff=$((diff + 1)); fi
            done
            echo "m2-5: $same pictures identical, $diff different (against $(basename "$REF"))" ;;
        m6-1) echo "m6-1:"; grep -h "^RESULT" "$R"/*.txt | sed 's/^/  /' ;;
        m6-4) echo "m6-4 (SDL): $(for f in "$R"/[1-8]-*.txt; do echo "$(basename "$f" .txt) $(grep -ho '[0-9.]* frames per second' "$f" | tail -1 | cut -d' ' -f1)"; done | tr '
' ' ')" ;;
        m6-7) echo "m6-7 (glmark2): $(grep -h 'glmark2 Score' "$R"/*.txt | sed 's/ *glmark2 //')" ;;
        esac
        ;;
    esac
done
