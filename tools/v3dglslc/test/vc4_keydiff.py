#!/usr/bin/env python3
"""Why a program compiled again at a draw on the Pi 3 (docs/PI3_PLAN.md §10, 2026-11-07).

A program's `.vc4p` holds the variants its link compiled; its `.vc4k` those its
draws asked for later (with the link's reference variants first). For each
variant in the `.vc4k` that the `.vc4p` lacks, this prints the key fields in which
it differs from the closest one the link has, and at the end a tally. Field names
follow v3dp/FORMAT.md 1.1's VC4_VARIANT key layout (tools/v3dglslc/vc4_variants.c,
r_fs_key and r_vs_key). Remember what ogles2_vc4_variant_find (ogles2/vc4/
program_vc4.c) leaves aside: a sampler's comparison function and its wraps (a
mirror-clamp wrap aside), so a difference in those alone is no miss.

    python3 tools/v3dglslc/test/vc4_keydiff.py <drawer with .vc4p and .vc4k files>

tests/a1200/p6_keydiff.sh runs a package on the card and fetches the drawer.
MIT licence (LICENSE: tools/v3dglslc).
"""
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc4_keys  # noqa: E402

FS = vc4_keys.FS_KEY
VS = 256 + 32 + 4  # the samplers, eight attribute formats, is_coord, the point size, clip_halfz, a pad
SAMPLER = {0: "format", 1: "format", 2: "format", 3: "format", 4: "swizzle", 5: "swizzle", 6: "swizzle",
           7: "swizzle", 8: "compare_mode", 9: "compare_func", 10: "wrap_s", 11: "wrap_t",
           12: "force_first_level", 13: "lod_emulation", 14: "aniso", 15: "half_signed"}
FS_FIELDS = {260: "depth", 261: "stencil", 262: "stencil_twoside", 263: "stencil_full_writemasks",
             264: "is_points", 265: "is_lines", 266: "point_coord_upper_left", 267: "msaa",
             268: "sample_coverage", 269: "alpha_to_coverage", 270: "alpha_to_one", 271: "logicop",
             280: "ucp_enables", 281: "blend", 282: "blend_rgb_func", 283: "blend_rgb_src",
             284: "blend_rgb_dst", 285: "blend_alpha_func", 286: "blend_alpha_src",
             287: "blend_alpha_dst", 288: "colormask", 289: "draw_buffer"}
VS_FIELDS = {288: "is_coord", 289: "per_vertex_point_size", 290: "clip_halfz"}


def field(stage, i):
    if i < 256:
        return "sampler%d.%s" % (i // 16, SAMPLER[i % 16])
    if stage == 4:
        if 256 <= i < 260:
            return "color_format"
        if 272 <= i < 276:
            return "point_sprite_mask"
        if 276 <= i < 280:
            return "ubo_1_size"
        return FS_FIELDS.get(i, "byte%d" % i)
    if i < 288:
        return "attr%d" % ((i - 256) // 4)
    return VS_FIELDS.get(i, "byte%d" % i)


def keys(path):
    """(fragment keys, vertex keys) of a file; coordinate variants left out."""
    fs, vs = [], []
    for t, b in vc4_keys.sections(path):
        if t != 7:
            continue
        stage = struct.unpack("<I", b[:4])[0]
        if stage == 4:
            fs.append(b[4:4 + FS])
        elif stage == 0:
            vs.append(b[4:4 + VS])
    return fs, vs


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().split("\n\n")[2])
        return 2
    tally = collections.Counter()
    files = sorted(glob.glob(os.path.join(sys.argv[1], "*.vc4k")))
    for k in files:
        p = k[:-1] + "p"
        name = os.path.basename(k)[:12]
        if not os.path.exists(p):
            print("%s: no .vc4p beside it" % name)
            continue
        (pf, pv), (kf, kv) = keys(p), keys(k)
        print("%s  link: %d fragment, %d vertex; the draws' file: %d fragment, %d vertex" %
              (name, len(pf), len(pv), len(kf), len(kv)))
        for stage, asked, held in ((4, kf, pf), (0, kv, pv)):
            for x in asked:
                if x in held or not held:
                    continue
                best = min(held, key=lambda y: sum(a != b for a, b in zip(x, y)))
                diff = sorted({field(stage, i) for i in range(min(len(x), len(best))) if x[i] != best[i]})
                what = "fragment" if stage == 4 else "vertex"
                print("    %-8s not in the link's file; the closest differs in: %s" % (what, ", ".join(diff)))
                tally[(what, tuple(diff))] += 1
    print("== variants the draws compiled, by what differs from the link's (%d programs)" % len(files))
    for (what, diff), n in tally.most_common():
        print("%3d %-8s %s" % (n, what, ", ".join(diff)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
