#!/bin/bash
# The VC4 key builder's test (keytest.c), on the PC, against the .vc4p files
# tests/pi3/vc4core/build.sh compiles (docs/PI3_PLAN.md §4.2, P5).
#   bash tests/pi3/keys/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HOME/ogles2-build-pi3/pi3}
gcc -O2 -Wall -Wextra -std=gnu99 "-I$REPO/include" "-I$REPO/ogles2/vc4" "-I$REPO/vc4core/include" -o "$OUT/keytest" \
    "$HERE/keytest.c" "$REPO/ogles2/vc4/key_vc4.c"
"$OUT/keytest" "$OUT/vc4p"
# Canonical keys (wrap modes, the comparison function left out) must give
# the same code as exact ones: the texture program's sampler states with
# each wrap mode, compiled from both key files; each exact variant's code
# (all but its key) must be one of the canonical file's.
V=${V:-$HOME/ogles2-build-pi3/v3dglslc/host/v3dglslc}
T=$(mktemp -d)
"$OUT/keytest" --write "$T"
for kind in exact canonical; do
    mkdir -p "$T/$kind"
    (cd "$REPO/tests/pi3/ref/shaders" && "$V" -g 21 -b pos,uv -k "$T/$kind.keys" -o "$T/$kind" texture.vert texture.frag > /dev/null 2>&1) \
        || echo "v3dglslc -k $kind failed"
done
python3 -I - "$T/exact/texture-texture.vc4p" "$T/canonical/texture-texture.vc4p" <<'PY'
import struct, sys
def variants(path):
    d = open(path, "rb").read()
    o, out = 32, []
    while o < len(d):
        t, n = struct.unpack("<II", d[o:o + 8])
        if t == 7:
            out.append(d[o + 8:o + 8 + n])
        o += 8 + ((n + 3) & ~3)
    return out
def code(v):
    stage = struct.unpack("<I", v[:4])[0]
    if stage == 4:
        return v[4 + 292:]
    o = 4 + 256 + 32 + 4
    n = struct.unpack("<I", v[o:o + 4])[0]
    return v[o + 4 + ((2 * n + 3) & ~3):]
ex, ca = variants(sys.argv[1]), variants(sys.argv[2])
cc = set(code(v) for v in ca)
missing = [i for i, v in enumerate(ex) if code(v) not in cc]
print("canonical keys: %d exact variants, %d canonical; %s" % (len(ex), len(ca),
      "every exact variant's code among the canonical ones" if not missing else
      "%d exact variant(s) with code of their own: %s" % (len(missing), missing)))
sys.exit(1 if missing else 0)
PY
rm -rf "$T"
