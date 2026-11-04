#!/bin/bash
# Switches the Raspberry Pi 3 between the two boot set-ups of PI3_PLAN §6.0
# and reboots it (the Pi, never the Amiga), then waits for it:
#   bash tests/pi3/boot.sh reference   Mesa's vc4 owns the GPU (KMS overlay)
#   bash tests/pi3/boot.sh bare        no driver on the 3D core, gpu_mem=128
#   bash tests/pi3/boot.sh status      which one is running
# Only two lines of config.txt change: the vc4-kms-v3d overlay is commented
# out or back in, and a "gpu_mem=128" line marked "# ogles2pi3" is added at
# the end (in [all]) or removed. The first switch keeps a copy of the
# original as config.txt.ogles2pi3-orig. sudo needs the Pi's password in
# $PI_PASS (or a password-less sudo).
set -eu
PI=${PI:-pi3}
MODE=${1:-status}
CFG=/boot/firmware/config.txt

state() {
    ssh "$PI" 'if [ -e /proc/device-tree/soc/v3d@7ec00000/status ] &&
        [ "$(tr -d "\0" < /proc/device-tree/soc/v3d@7ec00000/status)" = okay ]; then echo reference; else echo bare; fi;
        echo "gpu_mem $(vcgencmd get_mem gpu)"'
}

case $MODE in
status)
    state
    exit 0
    ;;
reference)
    EDIT='s/^#\(dtoverlay=vc4-kms-v3d\) # ogles2pi3 bare$/\1/; /^gpu_mem=128 # ogles2pi3$/d'
    ;;
bare)
    # The append comes before the delete: GNU sed still prints it when the
    # last line is the one deleted.
    EDIT='s/^\(dtoverlay=vc4-kms-v3d\)$/#\1 # ogles2pi3 bare/; $a gpu_mem=128 # ogles2pi3
/^gpu_mem=128 # ogles2pi3$/d'
    ;;
*)
    echo "usage: $0 reference|bare|status"
    exit 2
    ;;
esac

SUDO="sudo"
[ -n "${PI_PASS:-}" ] && SUDO="echo '$PI_PASS' | sudo -S -p ''"
ssh "$PI" "set -e; [ -f $CFG.ogles2pi3-orig ] || $SUDO cp $CFG $CFG.ogles2pi3-orig;
    $SUDO sed -i '$EDIT' $CFG; echo '--- config.txt now:'; grep -n -E 'vc4-kms-v3d|gpu_mem' $CFG"
echo "rebooting $PI"
ssh "$PI" "$SUDO systemctl reboot" || true
sleep 20
for k in $(seq 1 30); do
    if ssh -o ConnectTimeout=5 "$PI" true 2>/dev/null; then
        state
        exit 0
    fi
    sleep 5
done
echo "$PI did not come back within about 3 minutes"
exit 1
