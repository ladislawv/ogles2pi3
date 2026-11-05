#!/usr/bin/env python3
"""Which program of a package knocks the Pi 3 card's Wi-Fi out (PI3_PLAN §7,
P5 run 1: the link dropped after m2-5's run, transfers or not)? Runs a built
package's Run-* steps one at a time through amigent-ctl (each step a script of
its own: the package's setup, that one step, its cleanup), and after each asks
the station whether it still answers, while a ping log runs alongside.

  python tests/a1200/p5_netprobe.py PACKAGE [FIRST [LAST]]   (Git Bash or Windows)
The package must have been uploaded by a run before (tests/a1200/p5_run.sh).
"""
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CTL = os.environ.get("AMIGENT_CTL", r"C:\- CPP\amigent\.venv\Scripts\amigent-ctl.exe")
HOST = os.environ.get("AMIGENT_HOST", "192.168.1.128")
WORK = "Stuff:amigent/work"


def ctl(*args, timeout=600):
    env = dict(os.environ, AMIGENT_HOST=HOST)
    r = subprocess.run([CTL, *args], capture_output=True, text=True, env=env, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def steps(script):
    """The header (to the first step), the steps (If NOT {FROM} GT n VAL ... EndIf)
    and the footer (after the last)."""
    lines = open(script, encoding="latin-1").read().splitlines()
    starts = [i for i, l in enumerate(lines) if re.match(r"If NOT \{FROM\} GT \d+ VAL", l)]
    blocks = []
    for k, s in enumerate(starts):
        e = s
        while lines[e].strip() != "EndIf":
            e += 1
        blocks.append(lines[s + 1:e])
    head = [l for l in lines[:starts[0]] if not l.startswith((".key", ".def", ".bra", ".ket"))]
    end = e + 1
    return head, blocks, lines[end:]


def pinger(stop, log):
    while not stop.is_set():
        r = subprocess.run(["ping", "-n", "1", "-w", "1000", HOST], capture_output=True, text=True)
        ok = "Reply from %s" % HOST in r.stdout
        log.append((time.strftime("%H:%M:%S"), ok))
        time.sleep(1)


def main():
    name = sys.argv[1]
    pkg = os.path.join(HERE, "out", "ogles2-" + name)
    script = [f for f in os.listdir(pkg) if f.startswith("Run-")][0]
    head, blocks, foot = steps(os.path.join(pkg, script))
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    last = int(sys.argv[3]) if len(sys.argv) > 3 else len(blocks)
    remote = "%s/ogles2-%s" % (WORK, name)
    stop, log = threading.Event(), []
    threading.Thread(target=pinger, args=(stop, log), daemon=True).start()
    try:
        for n in range(first, last + 1):
            body = "\n".join(head + blocks[n - 1] + foot) + "\n"
            local = os.path.join(HERE, "out", "netprobe-step.txt")
            open(local, "w", encoding="latin-1", newline="\n").write(body)
            rc, out = ctl("put", local, "%s/NetProbe-Step" % remote)
            if rc:
                print("step %d: could not upload its script: %s" % (n, out.strip()))
                return 4
            title = next((l for l in blocks[n - 1] if "Echo \"Step" in l), "").strip()
            t0 = time.strftime("%H:%M:%S")
            rc, out = ctl("run", "-t", "300", "-d", remote, "Execute NetProbe-Step")
            time.sleep(5)
            up, info = ctl("up", "20")
            lost = [t for t, ok in log if t >= t0 and not ok]
            print("%s step %d %s: rc %s; station %s; ping lost %d since %s" %
                  (time.strftime("%H:%M:%S"), n, title[:60], rc, "answers" if up == 0 else "DOWN", len(lost), t0),
                  flush=True)
            if up != 0:
                print("the link died during or right after step %d" % n)
                return 4
    finally:
        stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
