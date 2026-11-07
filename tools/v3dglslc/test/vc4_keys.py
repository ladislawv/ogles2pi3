#!/usr/bin/env python3
"""v3dglslc -g 21 -k (docs/PI3_PLAN.md §4.2, PD7): variants compiled from keys
given in a file must be the variants a draw in that state compiles.

For each program below: compiled with -s (variants reached by drawing in
states), each VC4_VARIANT's key written to a key file (v3dglslc.c, -k: a
fragment record, then the vertex records whose fs_inputs it produces;
coordinate variants come with their vertex ones), compiled again with -k
and no -s, and the two files' variant sections compared as sets, byte for
byte (the other sections too).

Then the same with the records as ogles2.library makes them from a file it
holds (program.c, key_record: a program's .vc4k compiled anew with its keys
and a draw's): the fragment and vertex variants' keys in the file's order,
a vertex record's fs_inputs given as none (the compiler pairs it with the
fragment record before it).

Then EXT_draw_buffers' keys (§2.1, ogles2/vc4/job_vc4.c): a file holds a
fragment variant with draw_buffer k above 0 only for a gl_FragData[k] the
program writes (ogles2_vc4_output_for relies on it), and a key with draw_buffer
1 in a state the file lacks compiles into a variant with that key and that
output's code.

Then a link's hint records (ogles2_vc4_link_hints): the variants they add are the
ones the library's key builder asks for at a plain first draw.

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
    (None, "depth", "flat.vert", "mrt.frag"),
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


def key_file_library(path, out):
    """The records program.c's key_record writes: file order, coordinate
    variants left out, a vertex key's fs_inputs as a count of 0."""
    rec, nf, nv = b"", 0, 0
    for t, b in sections(path):
        if t != 7:
            continue
        stage = struct.unpack("<I", b[:4])[0]
        if stage == 4:
            rec += b[:4 + FS_KEY]
            nf += 1
        elif stage == 0:
            rec += b[:4 + 256 + 32 + 4] + struct.pack("<I", 0)
            nv += 1
    open(out, "wb").write(rec)
    return nf, nv


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


KEY_COLOR_FORMAT = 256  # after the samplers (key_vc4.c, vc4_key_fs), 32 bits
KEY_COLORMASK = FS_KEY - 4  # the key's last four bytes: colormask, draw_buffer, two of padding
KEY_DRAW_BUFFER = FS_KEY - 3


def fs_variants(path):
    return [b for t, b in sections(path) if t == 7 and struct.unpack("<I", b[:4])[0] == 4]


def draw_buffers(path):
    return sorted({b[4 + KEY_DRAW_BUFFER] for b in fs_variants(path)})


def check_draw_buffers(v, tmp):
    """0, or the number of checks that failed."""
    bad = 0
    d = os.path.join(tmp, "db")
    os.makedirs(d, exist_ok=True)
    for frag, want in (("flat.frag", [0]), ("mrt.frag", [0, 1])):
        got = draw_buffers(compile_(v, d, None, None, None, "flat.vert", frag))
        ok = got == want
        print("%-12s the file's draw_buffers %s: %s" % (frag, got, "as written" if ok else "WRONG, expected %s" % want))
        bad += not ok
    # A state the file lacks (no alpha written), through the on-demand path,
    # for each of the program's outputs: the variant asked for, its own code.
    path = compile_(v, d, None, None, None, "flat.vert", "mrt.frag")
    base = {b[4 + KEY_DRAW_BUFFER]: b for b in fs_variants(path)}
    keys, code = {}, {}
    for db in (0, 1):
        rec = bytearray(base[db][:4 + FS_KEY])
        rec[4 + KEY_COLORMASK] = 7
        keys[db] = bytes(rec)
        kf = os.path.join(tmp, "dbkeys.bin")
        open(kf, "wb").write(keys[db])
        e = os.path.join(tmp, "db%d" % db)
        os.makedirs(e, exist_ok=True)
        found = [b for b in fs_variants(compile_(v, e, None, None, kf, "flat.vert", "mrt.frag"))
                 if b[:4 + FS_KEY] == keys[db]]
        ok = len(found) == 1
        print("mrt.frag     a key for draw_buffer %d with colour mask 7: %s" %
              (db, "compiled" if ok else "NOT COMPILED"))
        bad += not ok
        if ok:
            code[db] = found[0][4 + FS_KEY:]
    if len(code) == 2:
        ok = code[0] != code[1]
        print("mrt.frag     the two outputs' code: %s" % ("differs" if ok else "THE SAME"))
        bad += not ok
    # A half-float target's two words (patch 0021) for each output: gl_FragData[1]
    # is 1 - color, so its variant does at least what gl_FragData[0]'s does. One
    # that dropped its colour is shorter (2026-11-06: the store compared the
    # output's location with its index, four times it, and only output 0 matched).
    sizes = {}
    for db in (0, 1):
        rec = bytearray(base[db][:4 + FS_KEY])
        rec[4 + KEY_COLOR_FORMAT:4 + KEY_COLOR_FORMAT + 4] = struct.pack("<I", 56)  # R16G16B16A16_FLOAT
        kf = os.path.join(tmp, "dbhdr.bin")
        open(kf, "wb").write(bytes(rec))
        e = os.path.join(tmp, "hdr%d" % db)
        os.makedirs(e, exist_ok=True)
        found = [b for b in fs_variants(compile_(v, e, None, None, kf, "flat.vert", "mrt.frag"))
                 if b[:4 + FS_KEY] == bytes(rec)]
        if len(found) == 1:
            sizes[db] = len(found[0])
    ok = len(sizes) == 2 and sizes[1] >= sizes[0]
    print("mrt.frag     half-float target, draw_buffer 0 and 1: %s bytes: %s" %
          (sizes, "each writes its colour" if ok else "DRAW_BUFFER 1 LOST ITS COLOUR"))
    bad += not ok
    # A half-float sampler holding negatives (patch 0022): eight lookups of
    # planes 0 to 7, against four of 0 to 3.
    path = compile_(v, d, "pos,uv", None, None, "texture.vert", "texture.frag")
    base_tex = fs_variants(path)[0]
    lengths = {}
    for signed in (0, 1):
        rec = bytearray(base_tex[:4 + FS_KEY])
        rec[4:8] = struct.pack("<I", 56)  # sampler 0: R16G16B16A16_FLOAT
        rec[4 + 15] = signed
        kf = os.path.join(tmp, "signed.bin")
        open(kf, "wb").write(bytes(rec))
        e = os.path.join(tmp, "signed%d" % signed)
        os.makedirs(e, exist_ok=True)
        found = [b for b in fs_variants(compile_(v, e, "pos,uv", None, kf, "texture.vert", "texture.frag"))
                 if b[:4 + FS_KEY] == bytes(rec)]
        if len(found) == 1:
            lengths[signed] = len(found[0])
    ok = len(lengths) == 2 and lengths[1] > lengths[0]
    print("texture.frag half-float sampler, unsigned and signed: %s bytes: %s" %
          (lengths, "the signed one reads more" if ok else "NOT LONGER: NO NEGATIVE PLANES"))
    bad += not ok
    return bad


KEY_DEPTH_ENABLED = KEY_COLOR_FORMAT + 4
BGRA8 = 105  # VC4K_B8G8R8A8_UNORM
HINT_KEYS_C = r"""
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "key_vc4.h"
/* hints.bin: what ogles2_vc4_link_hints sends; want.bin: the fragment keys a plain first
 * draw on a BGRA display builds, without and with a depth test: those the hints must give */
int main(void)
{
    unsigned char h[VC4_KEY_HINT_BYTES], k[VC4_KEY_FS_BYTES], kv[VC4_KEY_VS_BYTES];
    struct vc4_fs_state s;
    struct vc4_vs_state vs;
    FILE *f = fopen("hints.bin", "wb"), *w = fopen("want.bin", "wb"), *wv = fopen("want_vs.bin", "wb");
    unsigned char all[64];
    unsigned n;
    int depth;
    /* hints.bin: the library's own buffer for a BGRA display with a depth buffer; each
     * record a stage of 5 to 15 (the compiler refuses the rest); want_vs.bin: a vec4 at
     * slot 0 fed 3 floats */
    memset(all, 0xa5, sizeof(all));
    n = vc4_key_link_hints(all, sizeof(all), VC4K_B8G8R8A8_UNORM, 1);
    if (n != 3 * VC4_KEY_HINT_BYTES)
        return 1;
    for (depth = 0; depth < 3; depth++) /* the vertex hint, then the display's without and with depth */
        if (all[depth * VC4_KEY_HINT_BYTES] != (depth ? 5 : 6) || (depth && all[depth * VC4_KEY_HINT_BYTES + 8] != depth - 1))
            return 2;
    fwrite(all, 1, n, f);
    if (vc4_key_link_hints(h, sizeof(h), VC4K_B8G8R8A8_UNORM, 1) != 0 ||
        vc4_key_link_hints(all, sizeof(all), VC4K_R8G8B8A8_UNORM, 0) != VC4_KEY_HINT_BYTES)
        return 3;
    memset(&vs, 0, sizeof(vs));
    vs.nattrs = 1;
    vs.attr_format[0] = VC4K_R32G32B32_FLOAT;
    vc4_key_vs(kv, &vs, 1);
    fwrite(kv, 1, sizeof(kv), wv);
    fclose(wv);
    for (depth = 0; depth < 2; depth++) {
        memset(&s, 0, sizeof(s));
        s.color_format = VC4K_B8G8R8A8_UNORM;
        s.depth_test = (unsigned char)depth;
        s.colormask = 15;
        vc4_key_fs(k, &s, 1);
        fwrite(k, 1, sizeof(k), w);
    }
    fclose(f);
    fclose(w);
    return 0;
}
"""


def check_hints(v, tmp):
    """A link's hint records (ogles2/vc4/drawkey_vc4.c, ogles2_vc4_link_hints): the reference
    draw's variants plus one fragment variant a hint, whose key is the one the library's own
    builder makes for that draw (else the hint is wasted: the first draw compiles again);
    no new vertex variant; and a file without hints as before. 0, or the checks failed."""
    bad = 0
    d = os.path.join(tmp, "hints")
    os.makedirs(d, exist_ok=True)
    c = os.path.join(d, "hints.c")
    open(c, "w").write(HINT_KEYS_C)
    exe = os.path.join(d, "hints")
    subprocess.run(["gcc", "-O1", "-I" + os.path.join(REPO, "ogles2", "vc4"), "-I" + os.path.join(REPO, "vc4core", "include"),
                    "-o", exe, c, os.path.join(REPO, "ogles2", "vc4", "key_vc4.c")], check=True)
    subprocess.run([exe], cwd=d, check=True)
    want = open(os.path.join(d, "want.bin"), "rb").read()
    want = [want[:4 + FS_KEY], want[4 + FS_KEY:]]
    plain_dir, hint_dir = os.path.join(d, "plain"), os.path.join(d, "hinted")
    os.makedirs(plain_dir, exist_ok=True)
    os.makedirs(hint_dir, exist_ok=True)
    plain = compile_(v, plain_dir, None, None, None, "flat.vert", "flat.frag")
    hinted = compile_(v, hint_dir, None, None, os.path.join(d, "hints.bin"), "flat.vert", "flat.frag")
    pv = [b for t, b in sections(plain) if t == 7]
    hv = [b for t, b in sections(hinted) if t == 7]
    ok = len(hv) == len(pv) + 2 and all(x in hv for x in pv)
    print("flat         a link's hints add two variants to the reference's %d: %s" % (len(pv), "yes" if ok else "NO, %d" % len(hv)))
    bad += not ok
    new = [b for b in hv if b not in pv]
    ok = all(struct.unpack("<I", b[:4])[0] == 4 for b in new)
    print("flat         they are fragment variants: %s" % ("yes" if ok else "NO"))
    bad += not ok

    def same_for_library(a, b):
        """As ogles2_vc4_variant_find compares them: wraps and the comparison function aside."""
        a, b = bytearray(a), bytearray(b)
        for i in range(16):  # the samplers: compare_func and the wraps canonical
            for off in (9, 10, 11):
                a[4 + i * 16 + off] = b[4 + i * 16 + off] = 0
        return a == b
    for depth in (0, 1):
        found = [b for b in new if same_for_library(b[:4 + FS_KEY], want[depth])]
        ok = len(found) == 1
        print("flat         BGRA target, depth test %d: the library's own key %s" %
              (depth, "is in the file" if ok else "IS NOT IN THE FILE: THE HINT IS WASTED"))
        bad += not ok
    # A program's fragment-shader inputs do not change with the hint: its vertex variant serves it.
    ok = all(fs_inputs_of_fs(b) == fs_inputs_of_fs(pv[[struct.unpack("<I", x[:4])[0] for x in pv].index(4)]) for b in new)
    print("flat         the reference's vertex variant pairs with them: %s" % ("yes" if ok else "NO"))
    bad += not ok
    # The vertex hint: nothing for flat's vec3 (above: two variants, both fragment); for a vec4
    # position the vertex and coordinate variants with slot 0 three floats, the library's key.
    open(os.path.join(d, "pos4.vert"), "w").write("attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n")
    open(os.path.join(d, "pos4.frag"), "w").write(open(os.path.join(SHADERS, "flat.frag")).read())
    for sub, keys in (("pos4plain", None), ("pos4hinted", os.path.join(d, "hints.bin"))):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
        subprocess.run([v, "-g", "21"] + (["-k", keys] if keys else []) + ["-o", sub, "pos4.vert", "pos4.frag"],
                       cwd=d, check=True, capture_output=True)
    pv = [b for t, b in sections(os.path.join(d, "pos4plain", "pos4-pos4.vc4p")) if t == 7]
    hv = [b for t, b in sections(os.path.join(d, "pos4hinted", "pos4-pos4.vc4p")) if t == 7]
    new = [b for b in hv if b not in pv]
    stages = sorted(struct.unpack("<I", b[:4])[0] for b in new)
    ok = stages == [0, 1, 4, 4]
    print("pos4         hints add a vertex and a coordinate variant and the two fragment ones: %s" %
          ("yes" if ok else "NO, stages %s" % stages))
    bad += not ok
    want_vs = open(os.path.join(d, "want_vs.bin"), "rb").read()
    found = [b for b in new if struct.unpack("<I", b[:4])[0] == 0 and b[4:4 + 288] == want_vs[4:4 + 288] and
             b[4 + 289:4 + 291] == want_vs[4 + 289:4 + 291]]
    ok = len(found) == 1
    print("pos4         a vec4 position fed 3 floats: the library's own vertex key %s" %
          ("is in the file" if ok else "IS NOT IN THE FILE: THE HINT IS WASTED"))
    bad += not ok
    return bad


def check_order(v, tmp):
    """A file does not depend on the program compiled before it, in a compiler that outlives
    programs as the library's and the bridge's do: the driver's sampler views were kept, so a
    program's reference keys held the last one's samplers and no draw asked for them
    (2026-11-07, vc4_variants_drop_views). 0, or the checks failed."""
    bad = 0
    d = os.path.join(tmp, "order")
    alone = {}
    os.makedirs(os.path.join(d, "alone"), exist_ok=True)
    for vert, frag in (("flat.vert", "flat.frag"), ("texture.vert", "texture.frag")):
        alone[vert] = open(compile_(v, os.path.join(d, "alone"), None, None, None, vert, frag), "rb").read()
    together = os.path.join(d, "together")
    os.makedirs(together, exist_ok=True)
    subprocess.run([v, "-g", "21", "-o", together, "texture.vert", "texture.frag", "flat.vert", "flat.frag", "texture.vert", "texture.frag"],
                   cwd=SHADERS, check=True, capture_output=True)
    for name, vert in (("flat-flat", "flat.vert"), ("texture-texture", "texture.vert")):
        ok = open(os.path.join(together, name + ".vc4p"), "rb").read() == alone[vert]
        print("%-12s after another program: %s" % (name, "the same file" if ok else "A DIFFERENT FILE"))
        bad += not ok
    fs = [b for t, b in sections(os.path.join(together, "flat-flat.vc4p")) if t == 7 and struct.unpack("<I", b[:4])[0] == 4]
    ok = bool(fs) and all(not any(b[4:4 + 256]) for b in fs)
    print("flat         after a program with a sampler, its keys hold no sampler: %s" % ("yes" if ok else "NO"))
    bad += not ok
    return bad


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
            for how, make in (("-k", key_file), ("library's", key_file_library)):
                keys = os.path.join(tmp, "keys.bin")
                nf, nv = make(fa, keys)
                fb = compile_(v, b, bindings, None, keys, vert, frag)
                sa, sb = sections(fa), sections(fb)
                va = sorted(x for t, x in sa if t == 7)
                vb = sorted(x for t, x in sb if t == 7)
                others_same = [x for x in sa if x[0] != 7] == [x for x in sb if x[0] != 7]
                same = va == vb and others_same
                print("%-12s -s %-40s %2d fragment, %2d vertex keys (%s): %d variants with -s, %d from keys: %s" %
                      (vert + "+" + frag.split(".")[0], states or "(none)", nf, nv, how, len(va), len(vb),
                       "identical" if same else "DIFFERENT"))
                bad += not same
        bad += check_draw_buffers(v, tmp)
        bad += check_hints(v, tmp)
        bad += check_order(v, tmp)
    print("all identical" if not bad else "%d compile(s) differ" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
