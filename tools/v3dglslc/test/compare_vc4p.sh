#!/bin/bash
# The Raspberry Pi 3's programs (v3dglslc -g 21, docs/PI3_PLAN.md §4.5): the
# .vc4p files of tests/pi3/ref's programs, with -s states and with a -k key
# file, written by the native v3dglslc (S1, build_host.sh) and by the m68k
# one under vamos (S2, build_m68k.sh), must be identical byte for byte.
# The emulations' keys (patches 0016 to 0021: tests/pi3/vc4core/emu_keys.c,
# built by tests/pi3/vc4core/build.sh into $PI3) are compiled too.
#   usage: [HOSTV=host v3dglslc] compare_vc4p.sh
set -u
OUT=${OUT:-$HOME/ogles2-build-pi3/v3dglslc}
HOSTV=${HOSTV:-$OUT/host/v3dglslc}
PI3=${PI3:-$HOME/ogles2-build-pi3/pi3}
T="$(dirname "$(readlink -f "$0")")"
R="$(dirname "$(dirname "$(dirname "$T")")")"
W=$OUT/vc4pcmp && rm -rf $W && mkdir -p $W/host $W/m68k && cd $W
cp "$R"/tests/pi3/ref/shaders/*.vert "$R"/tests/pi3/ref/shaders/*.frag . || exit 1
sed -i 's/\r$//' *.vert *.frag
cp $OUT/v3dglslc.stripped v3dglslc

# The runs: name, then v3dglslc's arguments after -g 21 (-o added).
RUNS=(
  "plain|triangle.vert triangle.frag texture.vert texture.frag cube.vert cube.frag"
  "states|-s depth,blend,lines,stencil,rt565 flat.vert flat.frag"
  "textures|-b pos,uv -s tex565,texlum,tex4444,tex5551,texla,etc1 texture.vert texture.frag"
  "keys|-k flat.keys flat.vert flat.frag"
  "emutex|-b pos,uv -k emu.keys texture.vert texture.frag"
  "emuflat|-k flat_emu.keys flat.vert flat.frag"
  "emumrt|-k mrt.keys flat.vert mrt.frag"
)
cp "$PI3/emu.keys" "$PI3/flat_emu.keys" "$PI3/mrt.keys" . || echo "no emulation keys in $PI3: build tests/pi3/vc4core first"
# flat.keys: the keys of the states run's file (as vc4_keys.py writes them).
mkdir -p keysrc
$HOSTV -g 21 -s depth,blend,lines,stencil,rt565 -o keysrc flat.vert flat.frag > /dev/null 2>&1
python3 -I - keysrc/flat-flat.vc4p flat.keys "$T" <<'PY'
import sys
sys.path.insert(0, sys.argv[3])
import vc4_keys
vc4_keys.key_file(sys.argv[1], sys.argv[2])
PY

start=$(date +%s)
for run in "${RUNS[@]}"; do
  name=${run%%|*}; args=${run#*|}
  mkdir -p host/$name m68k/$name
  $HOSTV -g 21 -o host/$name $args > host/$name.txt 2>&1 || echo "host $name failed"
  timeout 3000 ~/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 4096 -- \
      v3dglslc -g 21 -o m68k/$name $args > m68k/$name.txt 2> m68k/$name.err || echo "m68k $name failed (rc $?)"
done
echo "m68k v3dglslc: $(( $(date +%s) - start )) s under vamos"

same=0 diff=0
for f in host/*/*.vc4p; do
  n=${f#host/}
  if cmp -s $f m68k/$n; then
    same=$((same + 1))
  else
    diff=$((diff + 1))
    echo "DIFFERENT: $n"; cmp -l $f m68k/$n 2>&1 | head -5
  fi
done
echo "$same identical, $diff different (of $(ls host/*/*.vc4p | wc -l))"
