#!/bin/bash
# Runs dEQP-GLES2 (built by tests/pi3/deqp/build.sh) on the Raspberry Pi 3 with
# Mesa's vc4, in the reference boot (PI3_PLAN §6.0): the hardware's own
# baseline (P0). Run in WSL:
#   bash tests/pi3/deqp/run.sh [NAME [CASES]]
# NAME names the run (default: main); CASES is a case pattern such as
# 'dEQP-GLES2.functional.fbo.*' (default: the whole must-pass list, 17,165
# cases). The options are the A1200 runs' (ports/deqp/README.md): 256x256,
# rgba8888d24s8ms0, no images or shader sources in the log; after the list,
# the multisample group again in a 4x MSAA configuration, as on the A1200.
# Each part runs in the background on the Pi (nohup), so a dropped ssh does
# not stop it. If the Pi stops in the middle (it rebooted on under-voltage in
# the first run, 2026-11-04), the run goes on from where it stopped in a new
# part (remaining.py; a case interrupted twice is given up). Logs:
# NAME.1.qpa, NAME.2.qpa, ..., NAME.ms4.qpa, named so that they sort in run
# order (summary.py keeps the last result of each case); they are copied back
# to $OUT/results/NAME/ and summarised there. RESUME=1 continues an earlier
# run of NAME instead of starting it again.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PI=${PI:-pi3}
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3/deqp}
NAME=${1:-main}
CASES=${2:-}
POLL=${POLL:-60}
DIR=pi3/deqp

rsync -a --delete --exclude results "$OUT/run/" "$PI:$DIR/"
scp -q "$HERE/remaining.py" "$PI:$DIR/"
ssh "$PI" "mkdir -p $DIR/results"
[ "${RESUME:-0}" = 1 ] || ssh "$PI" "cd $DIR/results && rm -f $NAME.*.qpa $NAME.*.out $NAME.done $NAME.list"

OPTS="--deqp-archive-dir=. --deqp-surface-width=256 --deqp-surface-height=256 --deqp-surface-type=pbuffer
--deqp-log-images=disable --deqp-log-shader-sources=disable --deqp-watchdog=enable"
OPTS=$(echo $OPTS)
if [ -n "$CASES" ]; then
    ssh "$PI" "cd $DIR && ./deqp-gles2 --deqp-runmode=stdout-caselist '--deqp-case=$CASES' |
        sed -n 's/^TEST: //p' > results/$NAME.list"
else
    ssh "$PI" "cd $DIR && cp gles2-main.txt results/$NAME.list"
fi

# Waits for the part's marker; returns 1 if the Pi rebooted (or stopped
# answering) instead.
wait_part() {
    local boot=$1 marker=$2 start=$(date +%s) b
    while :; do
        sleep "$POLL"
        b=$(ssh -o ConnectTimeout=10 "$PI" "uptime -s; test -f $marker && echo DONE" 2>/dev/null) || {
            echo "  $(( ($(date +%s) - start) / 60 )) min: no answer"
            continue
        }
        case $b in
        *DONE*) return 0 ;;
        esac
        [ "$(echo "$b" | head -1)" = "$boot" ] || { echo "  the Pi rebooted ($(echo "$b" | head -1))"; return 1; }
        echo "  $(( ($(date +%s) - start) / 60 )) min: $(ssh "$PI" "grep -c '^#beginTestCaseResult' $DIR/results/$NAME.*.qpa | tail -1")"
    done
}

START=$(date +%s)
for part in 1 2 3 4 5 6 7 8 9; do
    LEFT=$(ssh "$PI" "cd $DIR && python3 remaining.py results/$NAME.list results $NAME > results/$NAME.left 2> results/$NAME.left.log;
        cat results/$NAME.left.log >&2; wc -l < results/$NAME.left")
    [ "$LEFT" -gt 0 ] || break
    while ssh "$PI" "test -e $DIR/results/$NAME.$part.qpa"; do part=$((part + 1)); done
    echo "part $part: $LEFT cases"
    BOOT=$(ssh "$PI" "uptime -s")
    ssh "$PI" "cd $DIR && rm -f results/$NAME.part.done && cp results/$NAME.left results/$NAME.$part.list &&
        nohup sh -c './deqp-gles2 $OPTS --deqp-gl-config-name=rgba8888d24s8ms0 \
            --deqp-caselist-file=results/$NAME.$part.list --deqp-log-filename=results/$NAME.$part.qpa \
            > results/$NAME.$part.out 2>&1; touch results/$NAME.part.done' > /dev/null 2>&1 &"
    wait_part "$BOOT" "$DIR/results/$NAME.part.done" || continue
done

if [ -z "$CASES" ] && ! ssh "$PI" "test -e $DIR/results/$NAME.ms4.qpa"; then
    echo "multisample group in 4x MSAA"
    BOOT=$(ssh "$PI" "uptime -s")
    ssh "$PI" "cd $DIR && rm -f results/$NAME.part.done &&
        nohup sh -c './deqp-gles2 $OPTS --deqp-gl-config-name=rgba8888d24s8ms4 \
            --deqp-case=dEQP-GLES2.functional.multisample.* --deqp-log-filename=results/$NAME.ms4.qpa \
            > results/$NAME.ms4.out 2>&1; touch results/$NAME.part.done' > /dev/null 2>&1 &"
    wait_part "$BOOT" "$DIR/results/$NAME.part.done" || echo "  the MSAA part did not finish"
fi

mkdir -p "$OUT/results/$NAME"
rsync -a "$PI:$DIR/results/$NAME.*" "$OUT/results/$NAME/"
echo "done in $(( ($(date +%s) - START) / 60 )) min: $OUT/results/$NAME"
ssh "$PI" "vcgencmd get_throttled"
python3 "$REPO/ports/deqp/summary.py" "$OUT/results/$NAME"/*.qpa || true
