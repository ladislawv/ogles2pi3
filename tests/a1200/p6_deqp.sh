#!/bin/bash
# PI3_PLAN P6: dEQP-GLES2's must-pass list on the A1200 with the Pi 3 card, a group at a
# time, through amigent (p5_run.sh, package m7-1, Run-M1 LIST=<group>), each group's totals
# beside the last run of that group before on any card. The shaders are compiled on the PC:
# start tools/compile_bridge/compile_bridge.py (with --port, BRIDGE_PORT the same) in WSL first.
#   bash tests/a1200/p6_deqp.sh [group ...]     (default: every group, then multisample in 4x MSAA)
# A dropped Wi-Fi link stops a group's compiles and its run's reports while the Amiga runs on,
# which would count as failures: a group that lost its bridge or its station is waited for
# (WAIT, seconds, default 2700), the station rebooted if a program of the run is still going,
# and the group run again, up to TRIES times (default 3). The summary goes to
# tests/a1200/results/p6-deqp-<date>.txt as it runs.
# CHUNK=<cases>: a group longer than that runs in parts of so many cases, <group>-pNN, each
# tried on its own, so that a lost link costs one part (2026-11-06: the link dropped every 7 to
# 19 minutes, and fragment_ops, 47 minutes, never finished). WITH_MSAA=1: multisample in 4x MSAA
# after the named groups too (without names it always runs).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
PKG=$HERE/out/ogles2-m7-1
SUM=$RES/p6-deqp-$(date +%Y%m%d-%H%M).txt
CTL=${AMIGENT_CTL:-"$HOME/amigent-venv/bin/amigent-ctl"}
export AMIGENT_HOST=${AMIGENT_HOST:-192.168.1.128}
WAIT=${WAIT:-2700}
TRIES=${TRIES:-3}

say() { echo "$(date +%T) $*" | tee -a "$SUM"; }
up() { timeout 15 "$CTL" hello > /dev/null 2>&1; }
totals() { # a group's result .txt -> "passed/total failed notsup warnings"
    awk '/^ *Passed:/ {split($2, a, "/"); p = a[1]; t = a[2]}
         /^ *Failed:/ {split($2, a, "/"); f = a[1]}
         /^ *Not supported:/ {split($3, a, "/"); n = a[1]}
         /^ *Warnings:/ {split($2, a, "/"); w = a[1]}
         END {if (t != "") printf "%s/%s failed %s notsup %s warn %s", p, t, f, n, w; else printf "no totals"}' "$1"
}
before() { # group name -> its last earlier result .txt
    ls -d "$RES"/ogles2-m7-1-2[0-9]* 2>/dev/null | sort -r | while read -r d; do
        [ -f "$d/results/$1.txt" ] && grep -q "Test run totals" "$d/results/$1.txt" && { echo "$d/results/$1.txt"; break; }
    done
}
recover() { # the station back, nothing of the last run still going
    local t=0
    until up; do
        [ "$t" -ge "$WAIT" ] && { say "the station stayed away for $WAIT s: stopping"; exit 4; }
        sleep 20
        t=$((t + 20))
    done
    if timeout 25 "$CTL" run -t 15 Status 2> /dev/null | tr -d '\r' | grep -qE 'deqp-gles2|Run-M1'; then
        say "the run's program still goes: rebooting"
        timeout 20 "$CTL" reboot > /dev/null 2>&1
        sleep 20
        timeout 240 "$CTL" up 220 > /dev/null 2>&1 || { say "no answer after the reboot: stopping"; exit 4; }
    fi
    timeout 20 "$CTL" rm "Stuff:amigent/work/ogles2-m7-1/runpkg.lock" > /dev/null 2>&1
    rmdir "$HERE/out/p5_run.lock" 2> /dev/null
}
run() { # group, extra Run-M1 arguments, name
    local try=1 stamp new log old
    old=$(before "$3")
    while :; do
        stamp=$(date +%Y%m%d-%H%M%S)
        up || recover
        timeout 20 "$CTL" rm "Stuff:amigent/work/ogles2-m7-1/runpkg.lock" > /dev/null 2>&1
        ARGS="LIST=$1 NAME=$3${2:+ $2}" TIMEOUT=${TIMEOUT:-14400} bash "$HERE/p5_run.sh" m7-1 > /tmp/p6_deqp_one.log 2>&1
        new=$(ls -d "$RES"/ogles2-m7-1-2[0-9]* 2>/dev/null | tail -1)
        log=${new:+$new/results/ogles2.log}
        if [ -n "$new" ] && [[ ! "${new##*ogles2-m7-1-}" < "$stamp" ]] && grep -q "Test run totals" "$new/results/$3.txt" 2>/dev/null &&
           ! grep -q 'no bridge' "$log" 2> /dev/null; then
            say "$3: $(totals "$new/results/$3.txt")   before: $([ -n "$old" ] && totals "$old" || echo none)   ($(basename "$new"))"
            return 0
        fi
        say "$3: try $try did not finish cleanly ($(tail -1 /tmp/p6_deqp_one.log | cut -c1-90))"
        [ "$try" -ge "$TRIES" ] && { say "$3: given up after $TRIES tries"; return 1; }
        try=$((try + 1))
        recover
    done
}

[ -d "$PKG/caselists" ] || bash "$HERE/build_package.sh" m7-1 > /dev/null 2>&1
GROUPS_=${*:-$(ls "$PKG/caselists" | grep -v -- '-p[0-9][0-9]*\.txt$' | sed 's/\.txt$//')}
say "P6, dEQP-GLES2 must-pass on the Pi 3 card: $(echo $GROUPS_ | wc -w) groups, bridge port ${BRIDGE_PORT:-6803}${CHUNK:+, parts of $CHUNK cases}"
# The parts' lists, made first: build_package.sh makes the package anew for each run (m7-1/build.sh).
export P6_LISTS=$HERE/out/p6-lists
rm -rf "$P6_LISTS"
mkdir -p "$P6_LISTS"
for g in $GROUPS_; do
    n=$(grep -c . "$PKG/caselists/$g.txt" 2> /dev/null || echo 0)
    if [ -n "${CHUNK:-}" ] && [ "$n" -gt "$CHUNK" ]; then
        split -d -l "$CHUNK" --additional-suffix=.txt "$PKG/caselists/$g.txt" "$P6_LISTS/$g-p"
    fi
done
for g in $GROUPS_; do
    parts=$(ls "$P6_LISTS" | grep -E "^$g-p[0-9]+\.txt$" | sed 's/\.txt$//')
    for p in ${parts:-$g}; do
        run "$p" "" "$p"
    done
done
if [ -z "${*:-}" ] || [ "${WITH_MSAA:-}" = 1 ]; then
    run multisample "MSAA=1" multisample-msaa
fi
say "done: $SUM"
