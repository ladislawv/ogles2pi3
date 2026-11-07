#!/bin/bash
# The release archive tested as a user meets it (docs/PI3_PLAN.md §10, the 1.4
# betas): through amigent (as p5_run.sh), the archive from build_release.sh is
# uploaded, unpacked on the Amiga with its LhA, and its examples run with
# nothing installed and no compiler anywhere (tests/a1200/beta/Run-beta). Then
# the logs are checked: every program linked from Examples/shaders/, nothing
# compiled, no closest variant, every context's GPU memory returned.
#   bash tests/a1200/beta_run.sh            (Git Bash; either card)
# ARCHIVE: the .lha (default: the newest ogles2-<release>.lha the build made).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CTL=${AMIGENT_CTL:-"/c/- CPP/amigent/.venv/Scripts/amigent-ctl.exe"}
export AMIGENT_HOST=${AMIGENT_HOST:-$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['host'])" "$(cygpath -w "$REPO/tools/amictl/station.local")")}
export AMIGENT_RATE=${AMIGENT_RATE:-400}
DIR=Stuff:amigent/beta
if [ -z "${ARCHIVE:-}" ]; then
    A=$(MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 bash -c 'ls -t ~/ogles2-build-pi3/release/ogles2-*.lha | grep -v -e -sdk -e -sdl | head -1')
    ARCHIVE=$(MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 wslpath -w "$A" | tr -d '\r')
fi
NAME=$(basename "$(echo "$ARCHIVE" | tr '\\' /)")
OUT=$HERE/results/beta-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
ctl() { "$CTL" "$@" || { echo "$(date +%T) station: '$1' failed (status $?)"; exit 4; }; }
echo "$(date +%T) $NAME: uploading"
"$CTL" mkdir "$DIR" > /dev/null 2>&1
ctl put "$ARCHIVE" "$DIR/$NAME" > /dev/null
ctl put "$HERE/beta/Run-beta" "$DIR/Run-beta" > /dev/null
echo "$(date +%T) running the examples (about 3 minutes)"
"$CTL" run -t 900 -d "$DIR" "Stuff:amigent/piclock" 2>&1 | grep -E "board revision" | tee "$OUT/card.txt"
"$CTL" run -t 900 -d "$DIR" "Execute Run-beta $NAME $DIR" 2>&1 | tr -d '\r' | tee "$OUT/run.txt"
ctl get -r "$DIR/logs" "$OUT/logs" > /dev/null
echo "$(date +%T) the logs ($OUT/logs):"
bad=0
for f in "$OUT"/logs/*.txt; do
    n=$(basename "$f" .txt)
    links=$(grep -c "glLinkProgram: PROGDIR:shaders/" "$f")
    other=$(grep "glLinkProgram:" "$f" | grep -vc "glLinkProgram: PROGDIR:shaders/")
    compiled=$(grep -Ec "compiled|compiler|no binary" "$f")
    closest=$(grep -c "closest" "$f")
    freed=$(grep -c "context freed, 0 bytes of GPU memory still in use" "$f")
    printf '  %-20s %2d links from shaders/, %d other, %d compiled, %d closest, %d contexts freed\n' \
        "$n" "$links" "$other" "$compiled" "$closest" "$freed"
    [ "$links" -gt 0 ] && [ "$other$compiled$closest" = 000 ] && [ "$freed" -gt 0 ] || bad=1
done
[ $bad = 0 ] && echo "PASS: every example linked its binaries, nothing compiled" || echo "FAIL: see the logs"
exit $bad
