#!/bin/bash
# PI3_PLAN P5 (§7): test packages on the A1200 with the Pi 3 card, through
# amigent's amigentd (C:\- CPP\amigent, the fork with amictl's moves: port
# 6502, its amigent-ctl). Builds each package (tests/a1200/build_package.sh),
# puts the sync-logging library in (every log line written at once: a crash
# loses nothing), and has its programs compiled on the PC: a stand-in
# v3dglslc (a few bytes, never run: the library needs a compiler's file to ask
# for one; the real one is 19 MB) and v3dglslc-remote run from
# Stuff:amigent/bridge/ (compile_bridge.py must run in WSL). amigent-ctl
# uploads only what changed (CRC-32), runs the Run-* script on one command
# channel and prints a line a minute from another, so a station that stops
# answering shows at once; the batch stops at its exit status 4.
#   bash tests/a1200/p5_run.sh m2-4 [m1-5 ...]      (Git Bash, or inside WSL)
# AMIGENT_CTL: the amigent-ctl to use; AMIGENT_HOST: the A1200 (default: amictl's station.local);
# OGLES2_WSL: the WSL distribution, from Git Bash (as `wsl -l` names it);
# BRIDGE_PORT: the bridge's port on the station (default 6803). The newest
# connection to it wins, so another bridge on the network takes this run's
# compiles, with its own compiler: such a run needs a port of its own
# (compile_bridge.py --port the same; 2026-11-06: an older compiler's variants,
# from a bridge not yet identified, looked like a bug here).
set -u
BRIDGE_PORT=${BRIDGE_PORT:-6803}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
WREPO=$(echo "$REPO" | sed -E 's#^/([a-z])/#/mnt/\1/#')
STAND_IN='v3dglslc stand-in: the programs are compiled on the PC (tools/compile_bridge)'
# Inside WSL everything is here, amigent-ctl from a venv in the Linux home (no
# Python needed on Windows); from Git Bash the builds are WSL's and amigent-ctl
# is Windows'.
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    in_wsl() { "$@"; }
    CTL=${AMIGENT_CTL:-"$HOME/amigent-venv/bin/amigent-ctl"}
    REMOTE=$HOME/ogles2-build-pi3/m68k/amictl/v3dglslc-remote
    STATION=$REPO/tools/amictl/station.local
    PY=python3
else
    in_wsl() { MSYS_NO_PATHCONV=1 wsl -d "${OGLES2_WSL:-Ubuntu-24.04}" "$@"; }
    CTL=${AMIGENT_CTL:-"/c/- CPP/amigent/.venv/Scripts/amigent-ctl.exe"}
    REMOTE=$(in_wsl sh -c 'wslpath -w "$HOME/ogles2-build-pi3/m68k/amictl/v3dglslc-remote"' | tr -d '\r')
    STATION=$(cygpath -w "$REPO/tools/amictl/station.local")
    PY=python
fi
export AMIGENT_HOST=${AMIGENT_HOST:-$($PY -c "import json,sys;print(json.load(open(sys.argv[1]))['host'])" "$STATION")}
[ -n "$AMIGENT_HOST" ] || { echo "no station: AMIGENT_HOST, or tools/amictl/station.local's host"; exit 4; }
export AMIGENT_RESULTS=$RES
# KB/s for transfers (a weak Wi-Fi signal dropped the link, 2026-11-05: fixed at the AP; kept as a margin)
export AMIGENT_RATE=${AMIGENT_RATE:-400}
WORK=Stuff:amigent/work
BRIDGE=Stuff:amigent/bridge
CACHE=Stuff:amigent/pi3-cache
# One batch at a time: two share amigentd's four command channels and the
# build directories (2026-11-05: three ran at once, a fifth channel pushed one out)
LOCK=$HERE/out/p5_run.lock
mkdir -p "$HERE/out"
mkdir "$LOCK" 2> /dev/null || { echo "another p5_run.sh is running ($LOCK; remove it if none is)"; exit 1; }
trap 'rmdir "$LOCK" 2> /dev/null' EXIT
ctl() { "$CTL" "$@"; }
# amigent-ctl, the batch ended if the station is down (status 4)
need() {
    ctl "$@"
    local rc=$?
    if [ $rc = 4 ]; then
        echo "$(date +%T) STATION DOWN during: $*; batch stopped"
        exit 4
    fi
    return $rc
}
in_wsl pgrep -f compile_bridge.py > /dev/null ||
    { echo "no compile_bridge.py running: start python3 tools/compile_bridge/compile_bridge.py in WSL first"; exit 1; }
need mkdir "$BRIDGE" > /dev/null 2>&1
need put "$REMOTE" "$BRIDGE/v3dglslc-remote" > /dev/null
printf '%s\n' "$STAND_IN" > "$HERE/out/v3dglslc-stand-in"
need put "$HERE/out/v3dglslc-stand-in" "$BRIDGE/v3dglslc" > /dev/null
need run -t 30 "Protect $BRIDGE/v3dglslc-remote +rwed" > /dev/null
for t in "$@"; do
    echo "$(date +%T) $t: building"
    # The sync-logging library made here: not every package's build does (2026-11-07: m7-6 ran one
    # three hours old without a word).
    in_wsl bash -c "cd '$WREPO' && bash tests/a1200/build_package.sh $t && \
        make -j8 B=\$HOME/ogles2-build-pi3/m68k-sync EXTRA_CFLAGS=-DOGLES2_LOG_SYNC && \
        cp ~/ogles2-build-pi3/m68k-sync/ogles2.library tests/a1200/out/ogles2-$t/libs/" > /dev/null 2>&1 ||
        { echo "$t: build failed"; continue; }
    PKG=$HERE/out/ogles2-$t
    mkdir -p "$PKG/libs"
    # With out/ogles2-<package>.keep-bin the bin/ the station has stays, not uploaded or checked
    # (it is not removed there). Every run's upload reads each file back for its CRC: 36 MB of
    # disk for dEQP, at a moment the Wi-Fi link dropped too often (2026-11-06; PI3_PLAN §10).
    # Only while bin/ does not change: remove the file after a rebuild of the programs.
    [ -e "$HERE/out/ogles2-$t.keep-bin" ] && rm -rf "$PKG/bin"
    find "$PKG" -name v3dglslc -size +100k | while IFS= read -r f; do # (the path has a space)
        echo "$t: ${f#$PKG/} replaced by the stand-in for this run"
        printf '%s\n' "$STAND_IN" > "$f"
    done
    printf '%s\n' "$STAND_IN" > "$PKG/libs/v3dglslc"
    # the bridge's end on the Amiga, detached (Run >NIL:): it holds none of
    # amigentd's four command channels (an EXEC of its own did: the package's
    # EXEC, its heartbeat and the upload made a fifth push one out, 2026-11-05);
    # its output in T:p5-remote.txt
    stop_remote() { ctl run -t 30 'Break `Status COM=v3dglslc-remote` C' > /dev/null 2>&1; }
    stop_remote
    need start -d "$BRIDGE" "v3dglslc-remote >T:p5-remote.txt PORT=$BRIDGE_PORT WAIT NOFALLBACK TOKEN=Stuff:amictld/amictld.token" \
        "COMPILER=$BRIDGE/v3dglslc" > /dev/null
    # running on the Amiga before the package needs it (else its compiles wait 60 s each)
    up=0
    for i in 1 2 3 4 5 6; do
        sleep 2
        if ctl run -t 20 "Status" 2> /dev/null | grep -q "v3dglslc-remote"; then up=1; break; fi
    done
    if [ $up = 0 ]; then
        echo "$(date +%T) $t: the bridge's v3dglslc-remote did not start ($(ctl tail -n 300 T:p5-remote.txt 2>&1 | tr -d '\r' | tail -2))"
        continue
    fi
    # The programs compiled go to OGLES2Cache: (else T:, the RAM disk, whose
    # files the packages' memory checks would count as the library's): a
    # drawer on the station's disk, emptied first, assigned for this run.
    need mkdir "$CACHE" > /dev/null 2>&1
    need run -t 60 "Delete \"$CACHE/#?\" QUIET" > /dev/null
    need run -t 30 "Assign OGLES2Cache: \"$CACHE\"" > /dev/null
    stamp=$(date +%Y%m%d-%H%M%S)
    # A long batch (keep-bin): each run a new package cache/. Its script keeps every program compiled
    # there, two files each with the Pi 3's; never emptied, a drawer no 110 s could list had a compile's
    # 3 ms on the PC take 260 ms on the Amiga (2026-11-06, dEQP). Moved aside, an instant, not deleted:
    # delete $WORK/ogles2-$t-oldcache when the batch is done.
    # And the programs in RAM (Run-M1 RAMCACHE=1): no disk for each of them, about 750 files a part.
    if [ -e "$HERE/out/ogles2-$t.keep-bin" ]; then
        ctl mkdir "$WORK/ogles2-$t-oldcache" > /dev/null 2>&1
        ctl run -t 60 "Rename \"$WORK/ogles2-$t/cache\" \"$WORK/ogles2-$t-oldcache/c$stamp\"" > /dev/null 2>&1
        ARGS="${ARGS:+$ARGS }RAMCACHE=1"
    fi
    echo "$(date +%T) $t: uploading and running"
    # ARGS: the Run-* script's KEY=VALUE words (m6-5's FRAMES=0: the whole timedemo)
    ctl runpkg "$PKG" -t ${TIMEOUT:-1200} -w "$WORK" -r "$RES" ${ARGS:+-a "$ARGS"} 2>&1 |
        grep --line-buffered -E '^[0-9:]{8} |rc=|^results:|STATION DOWN|WARNING' | sed -u "s/^/   $t: /"
    rc=${PIPESTATUS[0]}
    stop_remote
    if [ "$rc" = 4 ]; then
        echo "$(date +%T) STATION DOWN during $t's run; batch stopped"
        exit 4
    fi
    need run -t 30 "Assign OGLES2Cache:" > /dev/null
    R=$(ls -d "$RES/ogles2-$t"-2[0-9]* 2>/dev/null | tail -1) # dated: not an old "-run3", which sorts after them
    if [ -z "$R" ] || [[ "${R##*ogles2-$t-}" < "$stamp" ]]; then
        echo "$(date +%T) $t: NO RESULTS from this run"
        continue
    fi
    echo "$(date +%T) $t: $(grep -h '^PASS' "$R"/results/*.txt 2>/dev/null | wc -l) PASS," \
        "$(grep -h '^FAIL:' "$R"/results/*.txt 2>/dev/null | wc -l) FAIL ($R)"
done
