#!/bin/bash
# The PC compile bridge on the station (CONTRIBUTING.md, "dEQP and the PC
# compile bridge"): a package's programs compiled on the PC instead of by the
# Amiga's v3dglslc, which matters most on the Pi 3 card (every program needs
# its VC4 variants) and for dEQP (hundreds of programs).
#   bash tests/a1200/bridge.sh start PACKAGE    (Git Bash; prints the job number)
#   bash tests/a1200/bridge.sh stop JOB
# start puts tests/a1200/out/ogles2-PACKAGE/bin/v3dglslc-remote (or the build's,
# when the package has none) into the package's drawer on the station and
# starts it there (WAIT NOFALLBACK: nothing compiles on the Amiga, the
# package's libs/v3dglslc only if the bridge is gone). compile_bridge.py must
# run in WSL first:  python3 tools/compile_bridge/compile_bridge.py
# OGLES2_WSL: the WSL distribution (as `wsl -l` names it; default Ubuntu-24.04).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
amictl() { python "$REPO/tools/amictl/amictl.py" "$@"; }
wsl() { command wsl -d "${OGLES2_WSL:-Ubuntu-24.04}" "$@"; }

case "${1:-}" in
start)
    NAME=ogles2-${2:?package name}
    BIN=$HERE/out/$NAME/bin/v3dglslc-remote
    if [ ! -f "$BIN" ]; then
        BIN=$(wsl wslpath -w '~/ogles2-build-pi3/m68k/amictl/v3dglslc-remote' 2>/dev/null | tr -d '\r')
        [ -n "$BIN" ] && [ -f "$BIN" ] || { echo "no v3dglslc-remote: make (it is in the Makefile's all)"; exit 1; }
    fi
    wsl pgrep -f compile_bridge.py > /dev/null ||
        { echo "no compile_bridge.py running: start python3 tools/compile_bridge/compile_bridge.py in WSL first"; exit 1; }
    P="$(amictl run CD | tr -d '\r' | tail -1)/$NAME" # runpkg's drawer: the server's work directory
    amictl mkdir "$P" > /dev/null 2>&1
    amictl mkdir "$P/bin" > /dev/null 2>&1
    amictl mkdir "$P/libs" > /dev/null 2>&1
    amictl put "$BIN" "$P/bin/v3dglslc-remote" > /dev/null || { echo "cannot put v3dglslc-remote"; exit 1; }
    amictl start -d "$P" bin/v3dglslc-remote WAIT NOFALLBACK "COMPILER=$P/libs/v3dglslc" | tail -1
    ;;
stop)
    amictl abort "${2:?job}" > /dev/null 2>&1
    amictl wait "$2" 30 | tail -1
    ;;
*)
    sed -n 2,12p "$0"
    exit 2
    ;;
esac
