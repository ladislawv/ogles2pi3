#!/usr/bin/env python3
"""The cases of a dEQP run still to do, for tests/pi3/deqp/run.sh (runs on the
Pi). A run is split into parts NAME.1.qpa, NAME.2.qpa, ... when the Pi stops
in the middle (a reboot, a hang): each part starts after the last case that has
a result. A case that was started without getting a result in two parts is
given up (it stays a "Crash" in summary.py).

    python3 remaining.py CASELIST RESULTS_DIR NAME > remaining.txt

Prints the remaining cases, one per line; on stderr, how many are done and
which cases were given up.
"""
import os
import re
import sys
from collections import Counter

BEGIN = re.compile(r"^#beginTestCaseResult (\S+)")
END = re.compile(r"^#(endTestCaseResult|terminateTestCaseResult)")


def main(caselist, results, name):
    part = re.compile(re.escape(name) + r"\.\d+\.qpa$")
    done, crashed = set(), Counter()
    for f in sorted(os.listdir(results)):
        if not part.match(f):
            continue
        current = None
        with open(os.path.join(results, f), encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = BEGIN.match(line)
                if m:
                    if current:
                        crashed[current] += 1
                    current = m.group(1)
                elif END.match(line) and current:
                    done.add(current)
                    current = None
        if current:
            crashed[current] += 1
    given_up = sorted(c for c, n in crashed.items() if n >= 2 and c not in done)
    left = 0
    with open(caselist) as fh:
        for line in fh:
            c = line.strip()
            if c and c not in done and c not in given_up:
                print(c)
                left += 1
    sys.stderr.write("%d done, %d left, %d given up%s\n" % (
        len(done), left, len(given_up), "".join("\n  " + c for c in given_up)))


if __name__ == "__main__":
    main(*sys.argv[1:4])
