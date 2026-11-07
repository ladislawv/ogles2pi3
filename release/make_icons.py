#!/usr/bin/env python3
"""The examples' Workbench icons (.info), drawn here: no image files, no libraries.

Each icon has a 4-colour image in the Workbench's first four pens (any AmigaOS)
and a GlowIcons image (AmigaOS 3.5 and newer, 3.2 among them: up to 255
colours, a glow round it when selected), 46 x 46 pixels. The programs' icons
are tools (a stack of 32 KB, their options as ToolTypes, switched off in
brackets: release/examples/wbstart.h reads them); the texts' are projects
shown by MultiView; the drawers' give their windows a size and place every
icon in it.

usage: make_icons.py examples OUTDIR     Examples/: the programs' icons, boing2026.txt's
       make_icons.py demos OUTDIR        the demos archive's drawer: the same and ReadMe's
       make_icons.py drawer OUTDIR NAME  NAME.info: a drawer's icon (for its parent)
       make_icons.py preview OUT.png     every image, 4x, for a look on the PC
MIT licence (LICENSE)."""
import math
import os
import struct
import sys
import zlib

W = H = 46
SS = 4  # samples per pixel per axis

# ---------------------------------------------------------------- drawing


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def boing(nx, ny, spin=0.0, tilt=-0.35):
    """The Boing ball's colour at a point of its disc (radius 1), lit from the top left; None outside"""
    rr = nx * nx + ny * ny
    if rr >= 1.0:
        return None
    nz = math.sqrt(1.0 - rr)
    c, s = math.cos(tilt), math.sin(tilt)
    x, y = nx * c - ny * s, nx * s + ny * c
    lon = math.atan2(x, nz) + spin
    lat = math.asin(max(-1.0, min(1.0, y)))
    check = (math.floor(lon / (math.pi / 8)) + math.floor(lat / (math.pi / 8))) & 1
    base = (225, 25, 30) if check else (245, 245, 245)
    lx, ly, lz = -0.45, -0.55, 0.70
    d = max(0.0, nx * lx + ny * ly + nz * lz)
    k = 0.30 + 0.70 * d
    col = tuple(min(255.0, v * k) for v in base)
    spec = max(0.0, d) ** 24
    return mix(col, (255, 255, 255), min(1.0, spec * 0.8))


def rounded(x, y, x0, y0, x1, y1, r):
    """Inside a rounded rectangle"""
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def edge(px, py, ax, ay, bx, by):
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)


def img_triangle(x, y):
    a, b, c = (23.0, 4.0), (3.5, 40.0), (42.5, 40.0)
    pts = [a, b, c]
    dmin = min(seg_dist(x, y, *pts[i], *pts[(i + 1) % 3]) for i in range(3))
    area = edge(*c, *a, *b)
    w0 = edge(x, y, *b, *c) / area
    w1 = edge(x, y, *c, *a) / area
    w2 = edge(x, y, *a, *b) / area
    inside = w0 >= 0 and w1 >= 0 and w2 >= 0
    if not inside and dmin > 1.2:
        return None
    if dmin <= 1.2:
        return (30, 30, 40)
    r, g, bl = (255, 70, 60), (60, 225, 70), (70, 100, 255)
    return tuple(w0 * r[i] + w1 * g[i] + w2 * bl[i] for i in range(3))


def img_cube(x, y):
    top, right, mid, left = (23.0, 4.0), (41.0, 13.5), (23.0, 23.0), (5.0, 13.5)
    bot, rbot, lbot = (23.0, 42.0), (41.0, 32.5), (5.0, 32.5)
    faces = [  # origin, u edge, v edge, shade, colours
        (top, (left[0] - top[0], left[1] - top[1]), (right[0] - top[0], right[1] - top[1]), 1.00),
        (left, (mid[0] - left[0], mid[1] - left[1]), (lbot[0] - left[0], lbot[1] - left[1]), 0.80),
        (mid, (right[0] - mid[0], right[1] - mid[1]), (bot[0] - mid[0], bot[1] - mid[1]), 0.60),
    ]
    outline = [top, right, rbot, bot, lbot, left]
    inner = [(left, mid), (mid, right), (mid, bot)]
    dmin = min(seg_dist(x, y, *outline[i], *outline[(i + 1) % 6]) for i in range(6))
    for o, u, v, shade in faces:
        det = u[0] * v[1] - u[1] * v[0]
        px, py = x - o[0], y - o[1]
        a = (px * v[1] - py * v[0]) / det
        b = (u[0] * py - u[1] * px) / det
        if -0.001 <= a <= 1.001 and -0.001 <= b <= 1.001:
            if dmin <= 1.1:
                return (25, 25, 35)
            if min(seg_dist(x, y, *p, *q) for p, q in inner) <= 0.6:
                return (60, 45, 30)
            ch = (int(a * 4) + int(b * 4)) & 1
            col = (250, 170, 40) if ch else (250, 245, 230)
            return tuple(v_ * shade for v_ in col)
    if dmin <= 1.1:
        return (25, 25, 35)
    return None


def img_boingocean(x, y):
    if not rounded(x, y, 1.5, 1.5, 44.5, 44.5, 6.0):
        return None
    if not rounded(x, y, 2.5, 2.5, 43.5, 43.5, 5.0):
        return (20, 30, 60)
    hz = 28.0
    b = boing((x - 23.0) / 12.5, (y - 17.0) / 12.5, spin=0.2)
    if b is not None:
        return b
    if y < hz:
        return mix((90, 150, 230), (200, 225, 250), y / hz)
    t = (y - hz) / (44.0 - hz)
    col = mix((40, 110, 190), (10, 40, 100), t)
    wave = math.sin(x * 0.55 + y * 1.7) * 0.5 + 0.5
    col = mix(col, (150, 200, 240), (wave ** 6) * (1.0 - t) * 0.9)
    # the ball's reflection
    rb = boing((x - 23.0) / 12.5, (2 * hz - y - 17.0 - 3.0) / 12.5, spin=0.2)
    if rb is not None:
        col = mix(col, rb, 0.30 * (1.0 - t))
    return col


def img_boing2026(x, y):
    if not rounded(x, y, 1.5, 1.5, 44.5, 44.5, 6.0):
        return None
    if not rounded(x, y, 2.5, 2.5, 43.5, 43.5, 5.0):
        return (10, 5, 25)
    hz = 29.0
    b = boing((x - 23.0) / 11.0, (y - 16.5) / 11.0, spin=0.6)
    if b is not None:
        return b
    if y < hz:  # the room's back wall: a dim neon grid
        col = mix((25, 10, 55), (55, 20, 90), y / hz)
        gx = abs(((x - 23.0) / 5.0) % 1.0 - 0.5)
        gy = abs((y / 5.0) % 1.0 - 0.5)
        if gx > 0.42 or gy > 0.42:
            col = mix(col, (60, 200, 255), 0.45)
        return col
    t = (y - hz) / (44.0 - hz)
    col = mix((40, 10, 70), (15, 5, 35), t)
    depth = 1.0 / max(0.05, (y - hz) / 15.0)
    u = (x - 23.0) / (y - hz + 1.0) * 6.0
    line = abs(u % 1.0 - 0.5) > 0.40 or abs((depth * 1.2) % 1.0 - 0.5) > 0.38
    if line:
        col = mix(col, (255, 60, 200), 0.85)
    rb = boing((x - 23.0) / 11.0, (2 * hz - y - 16.5 - 2.0) / 11.0, spin=0.6)
    if rb is not None:
        col = mix(col, rb, 0.35)
    # the ball's soft shadow
    d = math.hypot((x - 23.0) / 10.0, (y - 31.5) / 2.2)
    if d < 1.0:
        col = mix(col, (0, 0, 0), 0.55 * (1.0 - d))
    return col


def img_document(x, y):
    x0, y0, x1, y1, fold = 8.0, 3.0, 38.0, 43.0, 9.0
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return None
    if x - (x1 - fold) > y - y0:  # cut corner
        return None
    border = x < x0 + 1.0 or x > x1 - 1.0 or y < y0 + 1.0 or y > y1 - 1.0 or \
        abs((x - (x1 - fold)) - (y - y0)) < 0.8
    if border:
        return (40, 40, 50)
    if x > x1 - fold and y < y0 + fold:  # the fold
        return (200, 200, 205)
    if 12.0 <= x <= 34.0 and y >= 14.0 and y <= 39.0 and (y - 14.0) % 4.0 < 1.4:
        if not (y > 36.0 and x > 26.0):
            return (90, 100, 130)
    return (250, 250, 245)


def img_drawer(x, y):
    # a folder, its tab top left, a Boing ball on its front
    if 4.0 <= x <= 20.0 and 7.0 <= y <= 12.0:
        return (40, 30, 10) if (x < 5.0 or y < 8.0 or x > 19.0) else (215, 160, 55)
    if not (3.0 <= x <= 43.0 and 11.0 <= y <= 41.0):
        return None
    if x < 4.0 or x > 42.0 or y < 12.0 or y > 40.0:
        return (40, 30, 10)
    if y < 16.0:
        return (200, 145, 45)
    if y < 17.0:
        return (90, 60, 15)
    b = boing((x - 23.0) / 9.0, (y - 28.5) / 9.0, spin=0.3)
    if b is not None:
        return b
    return mix((245, 195, 80), (215, 160, 55), (y - 17.0) / 24.0)


def render(fn):
    """RGBA rows: each pixel the average of SS x SS samples"""
    rows = []
    for py in range(H):
        row = []
        for px in range(W):
            acc = [0.0, 0.0, 0.0]
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = fn(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                    if c is not None:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        n += 1
            if n * 3 < SS * SS:  # under a third covered: transparent (GlowIcons have no partial alpha)
                row.append(None)
            else:
                cov = n / (SS * SS)
                col = tuple(v / n for v in acc)
                # the edge blended toward the Workbench's grey
                row.append(tuple(int(round(v)) for v in mix((150, 150, 150), col, min(1.0, cov * 1.3))))
        rows.append(row)
    return rows


def glow(rows):
    """The selected image: a glow round the shape, the shape a little brighter"""
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            c = rows[y][x]
            if c is not None:
                row.append(tuple(min(255, int(v * 1.08 + 10)) for v in c))
                continue
            d = 9.0
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < H and 0 <= xx < W and rows[yy][xx] is not None:
                        d = min(d, math.hypot(dx, dy))
            if d <= 3.0:
                row.append(tuple(int(v) for v in mix((255, 250, 150), (255, 150, 30), (d - 1.0) / 2.0)))
            else:
                row.append(None)
        out.append(row)
    return out


# ---------------------------------------------------------------- colours


def median_cut(colours, n):
    boxes = [list(colours)]
    while len(boxes) < n:
        boxes.sort(key=lambda b: max(max(c[i] for c in b) - min(c[i] for c in b) for i in range(3)) if len(b) > 1 else -1)
        b = boxes.pop()
        if len(b) < 2:
            boxes.append(b)
            break
        ch = max(range(3), key=lambda i: max(c[i] for c in b) - min(c[i] for c in b))
        b.sort(key=lambda c: c[ch])
        m = len(b) // 2
        boxes += [b[:m], b[m:]]
    return [tuple(sum(c[i] for c in b) // len(b) for i in range(3)) for b in boxes if b]


def palettise(rows, maxcol=255):
    """Index 0 transparent; the colours from 1"""
    pix = [c for r in rows for c in r if c is not None]
    uniq = sorted(set(pix))
    pal = uniq if len(uniq) <= maxcol else median_cut(pix, maxcol)
    cache = {}

    def near(c):
        if c not in cache:
            cache[c] = 1 + min(range(len(pal)), key=lambda i: sum((pal[i][k] - c[k]) ** 2 for k in range(3)))
        return cache[c]
    idx = [[0 if c is None else near(c) for c in r] for r in rows]
    return [(0, 0, 0)] + pal, idx


WB_PENS = [(170, 170, 170), (0, 0, 0), (255, 255, 255), (102, 136, 187)]


def four_colour(rows):
    """The Workbench's first four pens (grey, black, white, blue), by brightness and colour, no
    dithering (at 46 x 46 it is only noise): dark is black, light white, a strong colour (the
    ball's red, the sea, the neon) blue, the rest grey; transparent = pen 0"""
    out = []
    for r in rows:
        row = []
        for c in r:
            if c is None:
                row.append(0)
                continue
            lum = 0.30 * c[0] + 0.59 * c[1] + 0.11 * c[2]
            sat = max(c) - min(c)
            if lum < 50:
                row.append(1)
            elif c[0] > c[1] + 70 and c[0] > c[2] + 70:  # the ball's red
                row.append(3)
            elif lum > 190:
                row.append(2)
            elif sat > 60:
                row.append(3)
            else:
                row.append(2 if lum >= 120 else 0)
        out.append(row)
    return out


# ---------------------------------------------------------------- .info


def packbits(data):
    out = bytearray()
    i = 0
    while i < len(data):
        j = i
        while j + 1 < len(data) and data[j + 1] == data[i] and j - i < 127:
            j += 1
        if j > i:
            out += bytes([257 - (j - i + 1), data[i]])
            i = j + 1
            continue
        j = i
        while j + 1 < len(data) and j - i < 127 and not (j + 2 < len(data) and data[j + 1] == data[j + 2]):
            j += 1
        out += bytes([j - i]) + bytes(data[i:j + 1])
        i = j + 1
    return bytes(out)


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def imag(pal, idx):
    data = packbits(bytes(v for r in idx for v in r))
    paldata = bytes(v for c in pal for v in c)
    hdr = struct.pack(">BBBBBBHH", 0, len(pal) - 1, 3, 1, 0, 8, len(data) - 1, len(paldata) - 1)
    return chunk(b"IMAG", hdr + data + paldata)


def glowicon(rows):
    pal1, idx1 = palettise(rows)
    sel = glow(rows)
    pal2, idx2 = palettise(sel)
    face = struct.pack(">BBBBH", W - 1, H - 1, 1, 0x11, max(len(pal1), len(pal2)) * 3 - 1)
    body = b"ICON" + chunk(b"FACE", face) + imag(pal1, idx1) + imag(pal2, idx2)
    return b"FORM" + struct.pack(">I", len(body)) + body


def planar(idx, depth=2):
    rowbytes = ((W + 15) // 16) * 2
    out = bytearray()
    for p in range(depth):
        for r in idx:
            bits = bytearray(rowbytes)
            for x, v in enumerate(r):
                if (v >> p) & 1:
                    bits[x >> 3] |= 0x80 >> (x & 7)
            out += bits
    return bytes(out)


def string(s):
    b = s.encode("ascii") + b"\0"
    return struct.pack(">I", len(b)) + b


WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_POS = -0x80000000


def info(kind, rows, pos=None, tool=None, tooltypes=(), stack=0, window=None):
    """A DiskObject: the 4-colour image (selected: complemented), the strings, the GlowIcon"""
    four = four_colour(rows)
    x, y = pos if pos else (NO_POS, NO_POS)
    gadget = struct.pack(">IhhhhHHHIIIIIHI", 0, 0, 0, W, H, 0x0004, 0x0003, 1, 1, 0, 0, 0, 0, 0, 1)
    d = struct.pack(">HH", 0xE310, 1) + gadget
    d += struct.pack(">BBIIiiIIi", kind, 0, 1 if tool else 0, 1 if tooltypes else 0, x, y,
                     1 if window else 0, 0, stack)
    assert len(d) == 78
    if window:  # OldDrawerData: a NewWindow, then the drawer's scroll position
        wx, wy, ww, wh = window
        d += struct.pack(">hhhhBBIIIIIIIhhHHH", wx, wy, ww, wh, 255, 255, 0, 0, 0, 0, 0, 0, 0, 90, 40,
                         0xFFFF, 0xFFFF, 1) + struct.pack(">ii", 0, 0)
    d += struct.pack(">hhhhhIBBI", 0, 0, W, H, 2, 1, 3, 0, 0) + planar(four)
    if tool:
        d += string(tool)
    if tooltypes:
        d += struct.pack(">I", (len(tooltypes) + 1) * 4) + b"".join(string(t) for t in tooltypes)
    if window:  # DrawerData's revision 1 part: flags, view mode (by icon)
        d += struct.pack(">IH", 0, 1)
    return d + glowicon(rows)


# ---------------------------------------------------------------- the icons

STACK = 32768
TOOLTYPES = {
    "boingocean": ["(FULLSCREEN)", "(WIDTH=640)", "(HEIGHT=480)", "(SCALE=1)"],
    "boing2026": ["(TOUR)", "(FULLSCREEN)", "(NOMSAA)", "(NOSOUND)", "(VSYNC)", "(WIDTH=640)", "(HEIGHT=480)",
                  "(SCALE=100)"],
}
IMAGES = {"triangle": img_triangle, "cube": img_cube, "boingocean": img_boingocean, "boing2026": img_boing2026,
          "document": img_document, "drawer": img_drawer}
VIEWER = "SYS:Utilities/MultiView"
# where each icon sits in its drawer's window: two rows
LAYOUT = {"triangle": (12, 6), "cube": (92, 6), "boingocean": (172, 6), "boing2026": (252, 6),
          "ReadMe": (12, 76), "boing2026.txt": (112, 76)}
_cache = {}


def rows_of(name):
    if name not in _cache:
        _cache[name] = render(IMAGES[name])
    return _cache[name]


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def programs(out):
    for p in ("triangle", "cube", "boingocean", "boing2026"):
        write(os.path.join(out, p + ".info"),
              info(WBTOOL, rows_of(p), LAYOUT[p], tooltypes=TOOLTYPES.get(p, ()), stack=STACK))
    write(os.path.join(out, "boing2026.txt.info"),
          info(WBPROJECT, rows_of("document"), LAYOUT["boing2026.txt"], tool=VIEWER))


def png(path, rows_list, scale=4):
    gap = 4
    w = len(rows_list) * (W + gap) * scale
    h = H * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for i, rows in enumerate(rows_list):
            for x in range((W + gap) * scale):
                c = rows[y // scale][x // scale] if x // scale < W else None
                raw += bytes(c if c else (170, 170, 170))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    body = b"\x89PNG\r\n\x1a\n"
    for cid, data in ((b"IHDR", hdr), (b"IDAT", zlib.compress(bytes(raw), 9)), (b"IEND", b"")):
        body += struct.pack(">I", len(data)) + cid + data + struct.pack(">I", zlib.crc32(cid + data) & 0xFFFFFFFF)
    write(path, body)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    what, out = sys.argv[1], sys.argv[2]
    if what == "preview":
        names = list(IMAGES)
        png(out, [rows_of(n) for n in names] + [glow(rows_of(n)) for n in names])
        pal = [palettise(rows_of(n)) for n in names]
        four = [[[WB_PENS[v] if rows_of(n)[y][x] is not None else None for x, v in enumerate(r)]
                 for y, r in enumerate(four_colour(rows_of(n)))] for n in names]
        png(out.replace(".png", "-4col.png"), four)
        print("colours:", {n: len(p[0]) - 1 for n, p in zip(names, pal)})
        return
    os.makedirs(out, exist_ok=True)
    if what == "examples":
        programs(out)
    elif what == "demos":
        programs(out)
        write(os.path.join(out, "ReadMe.info"), info(WBPROJECT, rows_of("document"), LAYOUT["ReadMe"], tool=VIEWER))
    elif what == "drawer":
        write(os.path.join(out, sys.argv[3] + ".info"),
              info(WBDRAWER, rows_of("drawer"), window=(60, 40, 340, 160)))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
