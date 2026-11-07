#!/bin/bash
# PI3_PLAN §10 (2026-11-07): why a package's programs compile again at a draw on the Pi 3 card.
# Runs the package with p5_run.sh (its OGLES2Cache: is emptied first, so every program is
# compiled by this run), fetches the .vc4p and .vc4k files the run left there, and prints
# tools/v3dglslc/test/vc4_keydiff.py's report: for each variant a draw compiled, what in its
# key differs from what the link compiled. Inside WSL, the compile bridge running:
#   BRIDGE_PORT=6804 bash tests/a1200/p6_keydiff.sh m2-5        (files into ~/keydiff/m2-5)
# The library's log counts the same thing: "variants for other draws, compiled".
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PKG=${1:?package name}
CTL=${AMIGENT_CTL:-"$HOME/amigent-venv/bin/amigent-ctl"}
export AMIGENT_HOST=${AMIGENT_HOST:-192.168.1.128}
CACHE=Stuff:amigent/pi3-cache # p5_run.sh's OGLES2Cache:
OUT=${OUT:-$HOME/keydiff/$PKG}
# a package's own cache/ (some Run-M1 assign one) would answer from earlier runs
"$CTL" run -t 60 "Delete \"Stuff:amigent/work/ogles2-$PKG/cache/#?\" QUIET" > /dev/null 2>&1
bash "$HERE/p5_run.sh" "$PKG" | tail -2
rm -rf "$OUT" && mkdir -p "$OUT"
"$CTL" run -t 60 "List $CACHE LFORMAT \"%n\"" 2> /dev/null | tr -d '\r' | grep -E '\.vc4[pk]$' > "$OUT/names.txt"
while read -r n; do
    "$CTL" get "$CACHE/$n" "$OUT/$n" > /dev/null 2>&1 || echo "could not fetch $n"
done < "$OUT/names.txt"
echo "$(ls "$OUT"/*.vc4p 2> /dev/null | wc -l) programs, $(ls "$OUT"/*.vc4k 2> /dev/null | wc -l) of them compiled at a draw ($OUT)"
python3 -B "$REPO/tools/v3dglslc/test/vc4_keydiff.py" "$OUT"
