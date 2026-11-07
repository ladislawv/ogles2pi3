#!/usr/bin/env python3
"""The release examples' Pi 3 keys (release/examples/vc4keys/<hash>.keys): probe R's key files
(ENV:OGLES2/Probe R writes T:ogles2-vc4keys/<hash>.keys, a fragment and a vertex record a draw
state, appended run after run) with each pair once, in the order first seen. build_release.sh
compiles each example's .vc4p with its file (v3dglslc -g 21 -k), so its draws on a Pi 3 find their
variants without the compiler (docs/PI3_PLAN.md §10, the 1.4 betas). Records: VC4_KEY_FS_BYTES and
VC4_KEY_VS_BYTES of ogles2/vc4/key_vc4.h.

usage: vc4keys.py OUTDIR KEYFILE...   (each written to OUTDIR under its own name)
MIT licence (LICENSE)."""
import os
import sys

PAIR = (4 + 292) + (4 + 296)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[2:]:
        data = open(path, "rb").read()
        if len(data) % PAIR:
            raise SystemExit("%s: %d bytes, not whole fragment and vertex records" % (path, len(data)))
        seen, keep = set(), []
        for i in range(0, len(data), PAIR):
            pair = data[i:i + PAIR]
            if pair not in seen:
                seen.add(pair)
                keep.append(pair)
        open(os.path.join(out, os.path.basename(path)), "wb").write(b"".join(keep))
        print("%s: %d draw states (%d recorded)" % (os.path.basename(path), len(keep), len(data) // PAIR))


if __name__ == "__main__":
    main()
