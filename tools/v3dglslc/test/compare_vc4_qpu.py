#!/usr/bin/env python3
"""Compares the VC4 QPU code of two VC4_DEBUG=qpu listings (Mesa's vc4
driver: "MESA_SHADER_<stage> prog a/b QPU:" followed by one 64-bit
instruction per line), stage by stage, taking each stage's last program in
each listing: e.g. v3dglslc -g 21 (WSL) against Mesa on the Raspberry Pi 3
(tests/pi3/ref/mesa_*.c with VC4_DEBUG=qpu). PI3_PLAN P4.

    python3 compare_vc4_qpu.py OURS.txt THEIRS.txt

Exit status 0 when every stage present in both is identical.
"""
import re
import sys

HEAD = re.compile(r"MESA_SHADER_(\w+) prog \d+/\d+ QPU:")
INST = re.compile(r"\b0x([0-9a-f]{16})\b")


def stages(path):
    """{stage: [instruction words]} of the last program of each stage."""
    out, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = HEAD.search(line)
        if m:
            cur = m.group(1)
            out[cur] = []
            continue
        m = INST.search(line)
        if m and cur:
            out[cur].append(m.group(1))
        elif cur and not line.strip().replace("MESA: info:", "").strip():
            cur = None
    return out


def main(a, b):
    sa, sb = stages(a), stages(b)
    bad = 0
    for st in sorted(set(sa) | set(sb)):
        if st not in sa or st not in sb:
            print("%-9s only in %s" % (st, a if st in sa else b))
            continue
        x, y = sa[st], sb[st]
        if x == y:
            print("%-9s identical (%d instructions)" % (st, len(x)))
        else:
            bad += 1
            print("%-9s DIFFERENT: %d against %d instructions" % (st, len(x), len(y)))
            for i in range(max(len(x), len(y))):
                p = x[i] if i < len(x) else "-" * 16
                q = y[i] if i < len(y) else "-" * 16
                print("   %3d %s %s %s" % (i, p, "==" if p == q else "!=", q))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
