#!/usr/bin/env python3
"""v3dglslc -g 21 -k (docs/PI3_PLAN.md §4.2, PD7): variants compiled from keys
given in a file must be the variants a draw in that state compiles.

For each program below: compiled with -s (variants reached by drawing in
states), each VC4_VARIANT's key written to a key file (v3dglslc.c, -k: a
fragment record, then the vertex records whose fs_inputs it produces;
coordinate variants come with their vertex ones), compiled again with -k
and no -s, and the two files' variant sections compared as sets, byte for
byte (the other sections too).

    python3 tools/v3dglslc/test/vc4_keys.py [--compiler PATH]

MIT licence (LICENSE: tools/v3dglslc).
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SHADERS = os.path.join(REPO, "tests", "pi3", "ref", "shaders")
PROGRAMS = [  # (bindings, states, vertex, fragment)
    (None, "depth,blend,lines,stencil,rt565", "flat.vert", "flat.frag"),
    (None, "points", "point.vert", "flat.frag"),
    ("pos,col", "ubyte1", "color.vert", "color.frag"),
    ("pos,uv", "tex565,texlum,tex4444,tex5551,texla,etc1", "texture.vert", "texture.frag"),
    ("pos,dir", "", "cube.vert", "cube.frag"),
]


def sections(path):
    data = open(path, "rb").read()
    secs, o = [], 32
    while o < len(data):
        t, n = struct.unpack("<II", data[o:o + 8])
        secs.append((t, data[o + 8:o + 8 + n]))
        o += 8 + ((n + 3) & ~3)
    return secs


FS_KEY = 256 + 4 + 12 + 4 + 4 + 1 + 9 + 2  # FORMAT.md 1.1: samplers, then the fragment fields


def vs_key_len(body):
    """A vertex or coordinate variant's key bytes (after the stage): the
    samplers, attr_formats, flags, fs_inputs."""
    o = 4 + 256 + 32 + 4
    n = struct.unpack("<I", body[o:o + 4])[0]
    return 256 + 32 + 4 + 4 + ((2 * n + 3) & ~3)


def fs_inputs_of_fs(body):
    """A fragment variant's prog-data inputs (they are a vertex key's fs_inputs)."""
    o = 4 + FS_KEY + 4 + 9 + 3 + 8
    n = struct.unpack("<I", body[o:o + 4])[0]
    return body[o:o + 4 + ((2 * n + 3) & ~3)]


def fs_inputs_of_vs(body):
    o = 4 + 256 + 32 + 4
    n = struct.unpack("<I", body[o:o + 4])[0]
    return body[o:o + 4 + ((2 * n + 3) & ~3)]


def key_file(path, out):
    fs = [b for t, b in sections(path) if t == 7 and struct.unpack("<I", b[:4])[0] == 4]
    vs = [b for t, b in sections(path) if t == 7 and struct.unpack("<I", b[:4])[0] == 0]
    rec = b"".join(b[:4 + FS_KEY] for b in fs)
    for v in vs:
        ins = fs_inputs_of_vs(v)
        match = [f for f in fs if fs_inputs_of_fs(f) == ins]
        if not match:
            raise SystemExit("%s: no fragment variant gives a vertex variant's inputs" % path)
        rec += match[0][:4 + FS_KEY] + v[:4 + vs_key_len(v)]
    open(out, "wb").write(rec)
    return len(fs), len(vs)


def compile_(v, outdir, bindings, states, keys, vert, frag):
    cmd = [v, "-g", "21"]
    if bindings:
        cmd += ["-b", bindings]
    if states:
        cmd += ["-s", states]
    if keys:
        cmd += ["-k", keys]
    cmd += ["-o", outdir, vert, frag]
    p = subprocess.run(cmd, cwd=SHADERS, capture_output=True, text=True)
    if p.returncode:
        raise SystemExit("v3dglslc failed: %s\n%s" % (" ".join(cmd), p.stderr[-2000:]))
    return os.path.join(outdir, "%s-%s.vc4p" % (vert.rsplit(".", 1)[0], frag.rsplit(".", 1)[0]))


def main():
    v = os.path.expanduser("~/ogles2-build-pi3/v3dglslc/host/v3dglslc")
    if "--compiler" in sys.argv:
        v = sys.argv[sys.argv.index("--compiler") + 1]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for bindings, states, vert, frag in PROGRAMS:
            a, b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
            os.makedirs(a, exist_ok=True)
            os.makedirs(b, exist_ok=True)
            fa = compile_(v, a, bindings, states, None, vert, frag)
            keys = os.path.join(tmp, "keys.bin")
            nf, nv = key_file(fa, keys)
            fb = compile_(v, b, bindings, None, keys, vert, frag)
            sa, sb = sections(fa), sections(fb)
            va = sorted(x for t, x in sa if t == 7)
            vb = sorted(x for t, x in sb if t == 7)
            others_same = [x for x in sa if x[0] != 7] == [x for x in sb if x[0] != 7]
            same = va == vb and others_same
            print("%-12s -s %-40s %2d fragment, %2d vertex keys: %d variants with -s, %d with -k: %s" %
                  (vert + "+" + frag.split(".")[0], states or "(none)", nf, nv, len(va), len(vb),
                   "identical" if same else "DIFFERENT"))
            bad += not same
    print("all identical" if not bad else "%d program(s) differ" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
