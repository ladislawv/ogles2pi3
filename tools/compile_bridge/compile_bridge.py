#!/usr/bin/env python3
"""The PC's side of v3dglslc-remote (tools/compile_bridge/v3dglslc_remote.c):
connects to it on the A1200, compiles each request ogles2.library sends
there with the host v3dglslc (SERVE: the same requests as frames on its
stdin, the same files byte for byte) and sends the written files back.
Ported from ogles3 (its frames carry other fields: these are "V2").

    python3 tools/compile_bridge/compile_bridge.py [--port 6803] [--compiler PATH]
    python3 tools/compile_bridge/compile_bridge.py --check

Port 6803: ogles3's bridge to the same station uses 6802, and the two must
not take each other's links. The station's address and token are amictl's
(station.local). It runs until
stopped (Ctrl-C), connecting again whenever the link ends. --check needs no
Amiga: the test programs as requests through the same path, each file
compared with the command line's: the Pi 4's (tools/v3dglslc/test/shaders,
version 1 LINK) and the Pi 3's (tests/pi3/ref/shaders, version 2 LINK and
LINK_BOUND, one with keys as ogles2.library sends a draw's).

Frames (big-endian u32s; a string is its length, 0xffffffff for NULL):
  bridge:  "V2H1", token                     (once, after connecting)
  A1200:   "V2OK"
  A1200:   "V2R1", version, op, device, vs, fs, bindings, keys, file name
  bridge:  "V2S1", result, log, file count, then each file's name and bytes

MIT licence (tools/amictl/LICENSE, as the station's).
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "amictl"))  # the station (submodule ladislawv/amictl)
import amictl  # noqa: E402  (its config: the station's address and token)

# v3dp/v3dglsl_port.h
V3DGLSL_LINK, V3DGLSL_QUIT, V3DGLSL_LINK_BOUND = 1, 2, 5
VERSION, VERSION_DEVICE = 1, 2
DEVICE_V3D42, DEVICE_VC4 = 42, 21
NULL = 0xFFFFFFFF


def default_compiler():
    return os.environ.get("V3DGLSLC", os.path.expanduser("~/ogles2-build-pi3/v3dglslc/host/v3dglslc"))


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def string(b):
    return u32(NULL) if b is None else u32(len(b)) + b


class Compiler:
    """The host v3dglslc in SERVE mode; started again if it ends, or when its
    file changes (a rebuilt compiler: the running one would answer for the
    old code, as one did for a whole dEQP group in ogles3, 2026-10-27)."""

    def __init__(self, path):
        self.path = path
        self.p = None
        self.mtime = None
        self.log = open(os.path.join(tempfile.gettempdir(), "compile_bridge.log"), "ab")

    def start(self):
        # A compiler being linked again cannot be started for a moment (the
        # link of 2026-11-05 dropped a dEQP run's link): waited for, up to a
        # minute, not taken for the end of the link.
        for _ in range(60):
            try:
                self.mtime = os.path.getmtime(self.path)
                self.p = subprocess.Popen([self.path, "SERVE"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=self.log)
                return
            except OSError:
                time.sleep(1)
        raise OSError("the host compiler %s cannot be started" % self.path)

    def restart(self):
        """Ended; the next request starts it again."""
        if self.p and self.p.poll() is None:
            self.p.stdin.close()
            self.p.wait()
        self.p = None

    def request(self, version, op, device, vs, fs, bindings, keys, out_path):
        """(result, log) of one request; the compiler writes out_path. A compiler
        that ends is started again and asked once more: one process fills its fake
        device's table of buffer objects (fake_drm.c's MAX_BOS: VC4 keeps every
        variant) and aborts, after thousands of compiles, which cost a dEQP case
        its variants (fragment_ops.random.25, 2026-11-06)."""
        for _ in range(2):
            if self.p and self.p.poll() is None and os.path.getmtime(self.path) != self.mtime:
                self.restart()
            if not self.p or self.p.poll() is not None:
                self.start()
            try:
                self.p.stdin.write(b"V2Q1" + u32(version) + u32(op) + u32(device) + string(vs) + string(fs) +
                                   string(bindings) + string(keys) + string(out_path.encode()))
                self.p.stdin.flush()
                head = self.p.stdout.read(12)
            except BrokenPipeError:
                head = b""
            if len(head) == 12 and head[:4] == b"V2A1":
                result, n = struct.unpack(">iI", head[4:])
                return result, self.p.stdout.read(n)
            self.p.kill()
            self.p.wait()
            self.p = None
            print("%s the host compiler ended: started again" % time.strftime("%H:%M:%S"), flush=True)
        return 2, b"compile_bridge: the host compiler ended"


def serve_request(comp, version, op, device, vs, fs, bindings, keys, name):
    """One request in a directory of its own: (result, log, [(name, bytes)])
    with the files the compiler wrote."""
    d = tempfile.mkdtemp(prefix="v2r")
    try:
        out_path = os.path.join(d, name) if name else os.path.join(d, "none.v3dp")
        result, log = comp.request(version, op, device, vs, fs, bindings, keys, out_path)
        files = []
        for f in sorted(os.listdir(d)):
            with open(os.path.join(d, f), "rb") as fh:
                files.append((f, fh.read()))
        return result, log, files
    finally:
        shutil.rmtree(d, ignore_errors=True)


def recv_all(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise ConnectionError("the A1200 closed the link")
        b += c
    return b


def recv_u32(s):
    return struct.unpack(">I", recv_all(s, 4))[0]


def recv_str(s):
    n = recv_u32(s)
    return None if n == NULL else recv_all(s, n)


def serve(host, port, token, comp):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"V2H1" + string(token.encode()))
    if recv_all(s, 4) != b"V2OK":
        raise ConnectionError("the A1200 refused the token")
    # Requests come when a program links. A link quiet for 90 s is made
    # again (ogles3, 2026-10-28: after an A1200 reset the keepalive did not
    # end the dead link under WSL, and an hour's compiles failed).
    s.settimeout(90)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if hasattr(socket, "TCP_KEEPIDLE"):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 30)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
    print("%s connected to %s:%d" % (time.strftime("%H:%M:%S"), host, port), flush=True)
    n = 0
    while True:
        if recv_all(s, 4) != b"V2R1":
            raise ConnectionError("an unreadable request")
        version, op, device = recv_u32(s), recv_u32(s), recv_u32(s)
        vs, fs, bindings, keys, name = (recv_str(s) for _ in range(5))
        if op == V3DGLSL_QUIT:  # a library expunge: a compiler started anew, as the A1200's would be
            comp.restart()
            s.sendall(b"V2S1" + u32(0) + string(b"") + u32(0))
            continue
        t0 = time.time()
        result, log, files = serve_request(comp, version, op, device, vs, fs, bindings, keys,
                                           name.decode("latin-1") if name else None)
        out = b"V2S1" + u32(result) + string(log) + u32(len(files))
        for f, b in files:
            out += string(f.encode()) + string(b)
        s.sendall(out)
        n += 1
        if n % 50 == 1 or result:
            print("%s request %d: op %d device %d -> %d, %d file(s), %.0f ms" %
                  (time.strftime("%H:%M:%S"), n, op, device, result, len(files), (time.time() - t0) * 1000),
                  flush=True)


def check(comp):
    """The test programs through serve_request against the command line's
    files, each in a compiler of its own (a program's default variants can
    depend on the one compiled before it in the same process)."""
    bad = n = 0
    ref = tempfile.mkdtemp(prefix="v2ref")
    cases = []  # (name, version, op, device, dir, vert, frag, cmd args, bindings, keys file)
    sh = os.path.join(REPO, "tools", "v3dglslc", "test", "shaders")
    for f in sorted(os.listdir(sh)):
        if f.endswith(".vert") and os.path.exists(os.path.join(sh, f[:-5] + ".frag")):
            cases.append((f[:-5], VERSION, V3DGLSL_LINK, DEVICE_V3D42, sh, f, f[:-5] + ".frag", [], None, None))
    p3 = os.path.join(REPO, "tests", "pi3", "ref", "shaders")
    cases.append(("vc4 flat", VERSION_DEVICE, V3DGLSL_LINK, DEVICE_VC4, p3, "flat.vert", "flat.frag",
                  ["-g", "21"], None, None))
    cases.append(("vc4 texture (bound)", VERSION_DEVICE, V3DGLSL_LINK_BOUND, DEVICE_VC4, p3, "texture.vert",
                  "texture.frag", ["-g", "21", "-b", "pos,uv"], b"0 pos\n1 uv\n", None))
    try:
        # a keyed request: the keys tools/clear_vc4/keys.c makes, against -k
        keys = os.path.join(ref, "clear.keys")
        kb = os.path.join(ref, "keys")
        r = subprocess.run(["gcc", "-O2", "-I" + os.path.join(REPO, "ogles2", "vc4"),
                            "-I" + os.path.join(REPO, "vc4core", "include"), "-o", kb,
                            os.path.join(REPO, "tools", "clear_vc4", "keys.c"),
                            os.path.join(REPO, "ogles2", "vc4", "key_vc4.c")], capture_output=True)
        if r.returncode == 0 and subprocess.run([kb, keys], capture_output=True).returncode == 0:
            cases.append(("vc4 clear (keys)", VERSION_DEVICE, V3DGLSL_LINK, DEVICE_VC4,
                          os.path.join(REPO, "tools", "clear_vc4"), "clear.vert", "clear.frag",
                          ["-g", "21", "-k", keys], None, keys))
        else:
            print("the key builder did not build: no keyed check")
            bad += 1
        for name, version, op, device, d, v, fr, args, bindings, kf in cases:
            out = os.path.join(ref, "cmd")
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(out)
            subprocess.run([comp.path] + args + ["-o", out, v, fr], cwd=d, capture_output=True)
            ext = ".vc4p" if device == DEVICE_VC4 else ".v3dp"
            want = os.path.join(out, "%s-%s%s" % (v[:-5], fr[:-5], ext))
            vs, fs = open(os.path.join(d, v), "rb").read(), open(os.path.join(d, fr), "rb").read()
            one = Compiler(comp.path)
            result, log, files = serve_request(one, version, op, device, vs, fs, bindings,
                                               open(kf, "rb").read() if kf else None, "p" + ext)
            one.restart()
            got = dict(files).get("p" + ext)
            same = os.path.exists(want) and got == open(want, "rb").read()
            n += 1
            if not same:
                bad += 1
                print("DIFFERS: %s (result %d, %s)" % (name, result, log.decode("latin-1").strip()[:200]))
    finally:
        shutil.rmtree(ref, ignore_errors=True)
    print("%d of %d programs identical to the command line's" % (n - bad, n))
    return 1 if bad or not n else 0


def main(argv):
    port, path = 6803, default_compiler()
    if "--port" in argv:
        port = int(argv[argv.index("--port") + 1])
    if "--compiler" in argv:
        path = argv[argv.index("--compiler") + 1]
    comp = Compiler(path)
    if "--check" in argv:
        return check(comp)
    cfg = amictl.config()
    while True:
        try:
            serve(cfg["host"], port, cfg["token"], comp)
        except KeyboardInterrupt:
            return 0
        except socket.timeout:  # quiet for 90 s: connected again, without a word
            time.sleep(1)
        except (OSError, ConnectionError) as e:
            print("%s %s; again in 5 s" % (time.strftime("%H:%M:%S"), e), flush=True)
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
