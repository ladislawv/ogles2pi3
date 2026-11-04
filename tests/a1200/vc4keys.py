#!/usr/bin/env python3
"""Summarises probe V's lines (ogles2/vc4keys.c, docs/PI3_PLAN.md §4.2, P2):
the VC4 shader keys each program's draws would need on the Raspberry Pi 3,
measured on the Pi 4.

    python3 tests/a1200/vc4keys.py ogles2.log [more logs...]

Per log: the context's totals; per program: its fragment and vertex keys,
how many first appeared from frame 30 on ("late": a stall on the Pi 3 while
the on-target compiler runs), and for programs with several keys the key
fields that differ between them (what PD7's options would take out of the
key). The key layouts are vc4keys.c's.
"""
import re
import sys
from collections import defaultdict

KEY = re.compile(r"vc4keys: program (\d+) (fragment|vertex) key (\d+) at frame (\d+): ([0-9a-f]*)")
PROG = re.compile(r"vc4keys: program (\d+) \((.*)\): (\d+) draws, (\d+) fragment keys \((\d+) late\), "
                  r"(\d+) vertex keys \((\d+) late\)")
CTX = re.compile(r"vc4keys: context: .*")

FS_FIELDS = (["primitive", "render target", "target format"] + ["target swizzle"] * 4 +
             ["blend on", "blend eq rgb", "blend eq alpha", "blend src rgb", "blend dst rgb",
              "blend src alpha", "blend dst alpha", "colour mask", "depth/stencil", "msaa/coverage"])


def fs_field(i):
    if i < len(FS_FIELDS):
        return FS_FIELDS[i]
    return "sampler %d %s" % ((i - len(FS_FIELDS)) // 5, "format" if (i - len(FS_FIELDS)) % 5 == 0 else "swizzle")


def vs_field(i, nslots):
    if i == 0:
        return "points with gl_PointSize"
    if i == 1:
        return "vertex inputs"
    if i < 2 + 3 * nslots:
        return "attribute %d %s" % ((i - 2) // 3, ("type", "size", "normalized")[(i - 2) % 3])
    j = i - 2 - 3 * nslots
    return "vertex sampler %d %s" % (j // 5, "format" if j % 5 == 0 else "swizzle")


def differing(keys, namer):
    width = max(len(k) for k in keys)
    padded = [k + b"\0" * (width - len(k)) for k in keys]
    out = []
    for i in range(width):
        if len(set(k[i] for k in padded)) > 1:
            name = namer(i, padded[0])
            if name not in out:
                out.append(name)
    return out


def report(keys, late, names, ctx_line, totals):
    hist = defaultdict(int)
    if ctx_line:
        print("  " + ctx_line)
    for prog in sorted(keys, key=int):
        fs, vs = keys[prog]["fragment"], keys[prog]["vertex"]
        hist[len(fs)] += 1
        totals["programs"] += 1
        totals["fs"] += len(fs)
        totals["vs"] += len(vs)
        totals["late"] += late[prog]["fragment"] + late[prog]["vertex"]
        line = "    program %-4s %-24s fragment %2d (late %d), vertex %2d (late %d)" % (
            prog, names.get(prog, "")[:24], len(fs), late[prog]["fragment"], len(vs), late[prog]["vertex"])
        if len(fs) > 1:
            line += "; fragment keys differ in: " + ", ".join(differing(fs, lambda i, k: fs_field(i)))
        if len(vs) > 1:
            line += "; vertex keys differ in: " + ", ".join(
                differing(vs, lambda i, k: vs_field(i, k[1] if len(k) > 1 else 0)))
        print(line)
    for n, c in hist.items():
        totals["hist"][n] += c


def main(paths):
    for path in paths:
        print("== %s" % path)
        totals = {"programs": 0, "fs": 0, "vs": 0, "late": 0, "hist": defaultdict(int)}
        keys = defaultdict(lambda: {"fragment": [], "vertex": []})
        late = defaultdict(lambda: {"fragment": 0, "vertex": 0})
        names = {}
        n = 0
        for line in open(path, encoding="latin-1"):
            m = KEY.search(line)
            if m:
                prog, stage, frame = m.group(1), m.group(2), int(m.group(4))
                keys[prog][stage].append(bytes.fromhex(m.group(5)))
                if frame >= 30:
                    late[prog][stage] += 1
                continue
            m = PROG.search(line)
            if m:
                names[m.group(1)] = m.group(2)
                continue
            m = CTX.search(line)
            if m:  # a context's end: its programs are reported
                n += 1
                print("  context %d" % n)
                report(keys, late, names, m.group(0), totals)
                keys.clear()
                late.clear()
                names.clear()
        if keys:
            print("  context %d (no end in the log)" % (n + 1))
            report(keys, late, names, None, totals)
        print("  all: %d programs, %d fragment keys, %d vertex keys, %d late; programs by fragment keys: %s" % (
            totals["programs"], totals["fs"], totals["vs"], totals["late"],
            ", ".join("%d: %d" % kv for kv in sorted(totals["hist"].items()))))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1])
        sys.exit(2)
    main(sys.argv[1:])
