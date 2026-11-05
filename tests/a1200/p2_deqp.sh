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
# The shaders are compiled on the PC (a run of hours on the A1200's own
# compiler is about 80 minutes so): the package's v3dglslc-remote is started
# on the station first, with NOFALLBACK (nothing compiles on the A1200), and
# needs the PC's side running in WSL beforehand:
#   python3 tools/compile_bridge/compile_bridge.py       (WSL; --check tests it alone)
# PC=0 compiles on the A1200 instead (to exercise the on-target compiler).
# Each group runs under tools/amictl/deqp_watch.py (amictld 1.4 on the
# station, a package with dEQP's progress patch): every 15 s a line with the
# cases done, the case running and for how long, the library's new log lines
# and the free memory, into $RES/p2-deqp-<date>-watch.txt; a hang or reset is
# reported with the case it happened in (35 s without an answer), given up
# after 5 minutes. WATCH=0 runs a group with amictl runpkg instead (blind).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
PKG=$HERE/out/ogles2-m7-1
SUM=$RES/p2-deqp-$(date +%Y%m%d-%H%M).txt
WLOG=${SUM%.txt}-watch.txt
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
    if [ "${WATCH:-1}" = 1 ]; then
        python "$REPO/tools/amictl/deqp_watch.py" "$PKG" "LIST=$1 NAME=$3${2:+ $2}" --timeout 14400 >> "$WLOG" 2>&1
        case $? in
        3) echo "$3: the A1200 hung or reset; the watch's last lines:" | tee -a "$SUM"
           tail -4 "$WLOG" | tee -a "$SUM"
           exit 1 ;;
        4) echo "$3: timed out (4 hours)" | tee -a "$SUM"; exit 1 ;;
        esac
    else
        amictl runpkg "$PKG" -t 14400 -a "LIST=$1 NAME=$3${2:+ $2}" > /dev/null
    fi
    new=$(ls -d "$RES"/ogles2-m7-1-* 2>/dev/null | tail -1)
    if [ "$new" = "$start" ] || ! grep -q "Test run totals" "$new/results/$3.txt" 2>/dev/null; then
        echo "$3: the run did not finish ($new)" | tee -a "$SUM"
        exit 1
    fi
    echo "$3: $(totals "$new/results/$3.txt")   before: $([ -n "$old" ] && totals "$old" || echo none) ($(basename "$(dirname "$(dirname "${old:-x/x/x}")")"))" | tee -a "$SUM"
}

GROUPS_=${*:-$(ls "$PKG/caselists" | sed 's/\.txt$//')}
echo "P2 acceptance, dEQP-GLES2 must-pass, $(date)" > "$SUM"
JOB=""
if [ "${PC:-1}" = 1 ]; then
    [ -f "$PKG/bin/v3dglslc-remote" ] || { echo "no $PKG/bin/v3dglslc-remote: build the package again"; exit 1; }
    JOB=$(bash "$HERE/bridge.sh" start m7-1) || { echo "$JOB"; exit 1; }
    echo "shaders compiled on the PC (v3dglslc-remote job $JOB)" | tee -a "$SUM"
    trap '[ -n "$JOB" ] && bash "$HERE/bridge.sh" stop "$JOB" > /dev/null' EXIT
fi
for g in $GROUPS_; do
    run "$g" "" "$g"
done
# the MSAA run: after every group, or with MSAA_TOO=1 after the ones named (a resumed run)
{ [ -z "${*:-}" ] || [ "${MSAA_TOO:-0}" = 1 ]; } && run multisample "MSAA=1" multisample-msaa
echo "done: $SUM"
