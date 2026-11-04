#!/bin/bash
# The Raspberry Pi 3's programs (v3dglslc -g 21, docs/PI3_PLAN.md §4.5): the
# .vc4p files of tests/pi3/ref's programs, with -s states and with a -k key
# file, written by the native v3dglslc (S1, build_host.sh) and by the m68k
# one under vamos (S2, build_m68k.sh), must be identical byte for byte.
#   usage: compare_vc4p.sh
set -u
OUT=${OUT:-$HOME/ogles2-build-pi3/v3dglslc}
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
)
# flat.keys: the keys of the states run's file (as vc4_keys.py writes them).
mkdir -p keysrc
$OUT/host/v3dglslc -g 21 -s depth,blend,lines,stencil,rt565 -o keysrc flat.vert flat.frag > /dev/null 2>&1
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
  $OUT/host/v3dglslc -g 21 -o host/$name $args > host/$name.txt 2>&1 || echo "host $name failed"
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
