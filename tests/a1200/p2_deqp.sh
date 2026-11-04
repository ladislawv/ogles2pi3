#!/bin/bash
# PI3_PLAN P2's acceptance (§5.1): dEQP-GLES2's must-pass list on the A1200,
# a group at a time (package ogles2-m7-1, Run-M1 LIST=<group>), each group's
# totals beside the last run of that group before (the final M7 run of
# 2026-10-14/15, unless a newer one exists), and multisample again in a 4x
# MSAA context (Run-M1 MSAA=1), as then. The package is built first:
#   bash tests/a1200/build_package.sh m7-1     (in WSL)
#   bash tests/a1200/p2_deqp.sh [group ...]    (Git Bash; default: every group)
# A summary goes to tests/a1200/results/p2-deqp-<date>.txt as it runs. Stops
# at the first group whose run does not finish (the station may need a reset:
# ask before one).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
PKG=$HERE/out/ogles2-m7-1
SUM=$RES/p2-deqp-$(date +%Y%m%d-%H%M).txt
amictl() { python "$REPO/tools/amictl/amictl.py" "$@"; }

totals() { # a group's result .txt -> "passed/total failed notsup warnings"
    awk '/^ *Passed:/ {split($2, a, "/"); p = a[1]; t = a[2]}
         /^ *Failed:/ {split($2, a, "/"); f = a[1]}
         /^ *Not supported:/ {split($3, a, "/"); n = a[1]}
         /^ *Warnings:/ {split($2, a, "/"); w = a[1]}
         END {if (t != "") printf "%s/%s failed %s notsup %s warn %s", p, t, f, n, w; else printf "no totals"}' "$1"
}
before() { # group name -> its last earlier result .txt
    ls -d "$RES"/ogles2-m7-1-* 2>/dev/null | sort -r | while read -r d; do
        [ -f "$d/results/$1.txt" ] && grep -q "Test run totals" "$d/results/$1.txt" && { echo "$d/results/$1.txt"; break; }
    done
}
run() { # group, extra Run-M1 arguments, name
    local start new
    start=$(ls -d "$RES"/ogles2-m7-1-* 2>/dev/null | tail -1)
    old=$(before "$3")
    amictl runpkg "$PKG" -t 14400 -a "LIST=$1 NAME=$3${2:+ $2}" > /dev/null
    new=$(ls -d "$RES"/ogles2-m7-1-* 2>/dev/null | tail -1)
    if [ "$new" = "$start" ] || ! grep -q "Test run totals" "$new/results/$3.txt" 2>/dev/null; then
        echo "$3: the run did not finish ($new)" | tee -a "$SUM"
        exit 1
    fi
    echo "$3: $(totals "$new/results/$3.txt")   before: $([ -n "$old" ] && totals "$old" || echo none) ($(basename "$(dirname "$(dirname "${old:-x/x/x}")")"))" | tee -a "$SUM"
}

GROUPS_=${*:-$(ls "$PKG/caselists" | sed 's/\.txt$//')}
echo "P2 acceptance, dEQP-GLES2 must-pass, $(date)" > "$SUM"
for g in $GROUPS_; do
    run "$g" "" "$g"
done
[ -z "${*:-}" ] && run multisample "MSAA=1" multisample-msaa
echo "done: $SUM"
