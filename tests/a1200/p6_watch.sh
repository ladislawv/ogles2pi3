#!/bin/bash
# PI3_PLAN P6: the watchdogs beside tests/a1200/p6_deqp.sh, whose own limit is 4 hours a group.
#
# On the Amiga, tools/netwatch: pings this PC and resets the Amiga when no reply comes for a minute
# (the Wi-Fi link drops and may stay down until a reset, and nothing here can reset it then). This
# script starts it, starts it again after each reboot, and stops it when p6_deqp.sh has ended: it is
# not in S:User-Startup, or with the PC off the Amiga would reset every minute. NETWATCH_PC: this
# PC's address on the Amiga's network (not WSL's).
#
# Here: every POLL seconds (60) the running group's result files on the station. No growth for STALL
# seconds (480) while a run is on, a new reset in netwatch's log, or netwatch gone from a station that
# answers (it does not end by itself: the machine restarted, even if the log's last line was lost): the
# PC's side of the run is killed (p6_deqp.sh then recovers and tries the group again), and this script
# exits, so that whoever started it hears of it at once. Exit status 3: a stall; 5: the Amiga was
# reset; 0: the batch ended. Start it again after 3 or 5: netwatch goes on meanwhile.
#   bash tests/a1200/p6_watch.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HOME/ogles2-build-pi3/m68k/amictl/netwatch
CTL=${AMIGENT_CTL:-"$HOME/amigent-venv/bin/amigent-ctl"}
export AMIGENT_HOST=${AMIGENT_HOST:-192.168.1.128}
PC=${NETWATCH_PC:-192.168.1.64}
W=Stuff:amigent/work/ogles2-m7-1/results
BR=Stuff:amigent/bridge
NLOG=Stuff:amigent/netwatch.log
POLL=${POLL:-60}
STALL=${STALL:-480}
SUM=$(ls -t "$HERE"/results/p6-deqp-*.txt 2> /dev/null | grep -v -- -watch | head -1)
LOG=${SUM%.txt}-watch.txt
last=""
since=$(date +%s)
down=0
note() { echo "$(date +%T) $*" >> "$LOG"; }
resets() { timeout 30 "$CTL" tail -n 20000 "$NLOG" 2> /dev/null | tr -d '\r' | grep -c 'resetting'; }
kill_run() { # the PC's side: a client that lost its station may ignore a TERM
    pkill -f 'amigent-ctl runpkg'
    sleep 3
    pkill -9 -f 'amigent-ctl runpkg'
}
# The same for clients older than netwatch's reset line ("... min M sec S: ..."; the Amiga's clock is
# this PC's): a run begun after the reset is alive, and this script may hear of the reset minutes late
# (2026-11-06: it killed a healthy run that way). A line without a time: all of them.
kill_older_than_reset() {
    local m s age p e
    m=$(echo "$1" | sed -nE 's/.* min ([0-9]+) sec ([0-9]+):.*/\1/p')
    s=$(echo "$1" | sed -nE 's/.* min ([0-9]+) sec ([0-9]+):.*/\2/p')
    [ -n "$m" ] && [ -n "$s" ] || { kill_run; return; }
    age=$(($(date +%s) - $(date -d 00:00 +%s) - m * 60 - s))
    [ "$age" -gt 0 ] || { kill_run; return; }
    for p in $(pgrep -f 'amigent-ctl runpkg'); do
        e=$(ps -o etimes= -p "$p" 2> /dev/null)
        if [ "${e:-0}" -ge "$age" ]; then
            kill "$p" 2> /dev/null
            sleep 3
            kill -9 "$p" 2> /dev/null
        fi
    done
}
seen=0
ensure_netwatch() { # 0: it runs, or was only now started the first time; 1: it had gone (and runs again)
    local st rc=0
    st=$(timeout 30 "$CTL" run -t 20 Status 2> /dev/null | tr -d '\r') || return 0 # no answer: next time
    if echo "$st" | grep -q netwatch; then
        seen=1
        return 0
    fi
    [ "$seen" = 1 ] && rc=1
    seen=1
    timeout 90 "$CTL" put "$OUT" "$BR/netwatch" > /dev/null 2>&1
    timeout 20 "$CTL" run -t 15 "Protect $BR/netwatch +rwed" > /dev/null 2>&1
    timeout 20 "$CTL" start -d "$BR" "netwatch $PC LOG=$NLOG >T:netwatch.txt" > /dev/null 2>&1
    note "netwatch started, pinging $PC"
    return $rc
}
note "watch: every $POLL s, a stall after $STALL s, on $(basename "$SUM")"
known=$(resets)
known=${known:-0}
while pgrep -f 'p6_deqp\.sh' > /dev/null; do
    if ! timeout 15 "$CTL" hello > /dev/null 2>&1; then
        down=$((down + 1))
        [ $((down % 5)) = 1 ] && note "the station does not answer"
        since=$(date +%s)
        last=""
        sleep "$POLL"
        continue
    fi
    down=0
    ensure_netwatch
    gone=$?
    n=$(resets)
    if [ "${n:-0}" -gt "$known" ] || [ "$gone" = 1 ]; then
        line=$(timeout 30 "$CTL" tail -n 20000 "$NLOG" 2> /dev/null | tr -d '\r' | grep 'resetting' | tail -1)
        [ "${n:-0}" -gt "$known" ] || line="netwatch had gone: the machine restarted (no reset line in its log)"
        note "NETWATCH RESET the Amiga ($line)"
        echo "$(date +%T) NETWATCH RESET the Amiga: $line" >> "$SUM"
        echo "NETWATCH RESET the Amiga: $line"
        kill_older_than_reset "$line"
        exit 5
    fi
    if ! pgrep -f 'amigent-ctl runpkg' > /dev/null; then
        since=$(date +%s) # between groups: building, uploading, recovering
        last=""
        sleep "$POLL"
        continue
    fi
    now=$(timeout 30 "$CTL" ls "$W" 2> /dev/null | tr -d '\r' | awk '$1 == "F" {s += $2; n = $NF ~ /\.qpa$/ ? $NF : n} END {if (NR) print s, n}')
    if [ -n "$now" ] && [ "$now" != "$last" ]; then
        last=$now
        since=$(date +%s)
        note "$now"
    elif [ $(($(date +%s) - since)) -ge "$STALL" ]; then
        group=${last##* }
        case=$(timeout 30 "$CTL" tail -n 600 "$W/${group%.qpa}.txt" 2> /dev/null | tr -d '\r' | grep -E 'Test case' | tail -1)
        note "STALL: no growth for $STALL s in $group; the last case: $case; the run killed"
        echo "$(date +%T) STALL in $group: $case" >> "$SUM"
        echo "STALL in $group: $case"
        kill_run
        exit 3
    fi
    sleep "$POLL"
done
timeout 30 "$CTL" run -t 20 'Break `Status COM=netwatch` C' > /dev/null 2>&1
note "p6_deqp.sh has ended; netwatch stopped"
echo "p6_deqp.sh has ended"
