#!/bin/bash
# PI3_PLAN P2 (§5.1, "Measuring speed between steps"): a step's library
# against the last commit's, alternating on the A1200, RUNS times each (3):
# gears (m6-1, TEETH=1: bitmap immediate, arrays, list, window, screen) and
# Quake III's timedemo (package p2v-m6-5, from tests/a1200/p2_vc4keys.sh).
# The reference is built from HEAD in a tree of its own (git archive, its own
# B), so it shares nothing with the working tree's build:
#   bash tests/a1200/p2_speed.sh [ref]      builds ~/p2head-build from HEAD
#   bash tests/a1200/p2_speed.sh [RUNS]     runs (the reference must exist)
#   REF_LIB=/path/ogles2.library bash ...   against another library (P2's acceptance: 1.3.3's)
# Run from Git Bash in the repository, the working tree's library built
# (~/ogles2-build-pi3/m68k/ogles2.library) and m6-1 packaged once
# (tests/a1200/p2_checks.sh m6-1 does both).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
WREPO=$(echo "$REPO" | sed -E 's#^/([a-z])/#/mnt/\1/#')
wsl() { MSYS_NO_PATHCONV=1 command wsl -d "${OGLES2_WSL:-Ubuntu-24.04}" "$@"; } # OGLES2_WSL: as `wsl -l` names it

if [ "${1:-}" = ref ]; then
    wsl bash -c "set -e; rm -rf ~/p2head ~/p2head-build; mkdir -p ~/p2head
        cd '$WREPO' && git archive HEAD | tar -x -C ~/p2head && git log --oneline -1
        cd ~/p2head && make -j16 B=\$HOME/p2head-build/m68k \$HOME/p2head-build/m68k/ogles2.library > /tmp/p2head.log 2>&1 ||
            { tail -20 /tmp/p2head.log; exit 1; }
        ls -l \$HOME/p2head-build/m68k/ogles2.library"
    exit $?
fi
RUNS=${1:-3}
WHOME=$(wsl sh -c 'echo $HOME' | tr -d '\r') # the WSL home (its user differs between machines)
REF=$(wsl wslpath -w "${REF_LIB:-$WHOME/p2head-build/m68k/ogles2.library}" | tr -d '\r') # REF_LIB: another (a WSL path)
NEW=$(wsl wslpath -w "$WHOME/ogles2-build-pi3/m68k/ogles2.library" | tr -d '\r')
[ -f "$REF" ] || { echo "no reference library: bash $0 ref"; exit 1; }
amictl() { python "$REPO/tools/amictl/amictl.py" "$@"; }
newest() { ls -d "$RES"/$1-* 2>/dev/null | tail -1; }

gears() { # library label
    cp "$1" "$HERE/out/ogles2-m6-1/libs/ogles2.library" || exit 1
    amictl runpkg "$HERE/out/ogles2-m6-1" -t 1200 > /dev/null
    echo "gears $2: $(grep -h '^RESULT' "$(newest ogles2-m6-1)"/results/*.txt | grep 'TEETH=1 ' |
        sed -E 's/.*gl4es +([A-Z]+) +([a-z]+).*= *([0-9.]+) fps/\2-\1 \3/' | tr '\n' ' ')"
}
q3() {
    cp "$1" "$HERE/out/p2v-m6-5/libs/ogles2.library" || exit 1
    amictl runpkg "$HERE/out/p2v-m6-5" -t 1800 -a "FRAMES=0" > /dev/null
    echo "q3 $2: $(grep -h 'frames.*fps' "$(newest p2v-m6-5)"/results/qconsole.log | sed -E 's/.* ([0-9.]+) fps.*/\1/') fps"
}
for i in $(seq "$RUNS"); do gears "$REF" ref; gears "$NEW" new; done
for i in $(seq "$RUNS"); do q3 "$REF" ref; q3 "$NEW" new; done
