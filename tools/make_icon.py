#!/usr/bin/env python3
"""Draw the catalogue icon (512x512 PNG) in the app's own style: a spectrum
trace over a waterfall, using the app's colour map. Also writes a 64x64
preview, the size Keira shows it at. No imaging library needed."""
import math
import os
import random
import struct
import sys
import zlib

S = 512
STOPS = [(0, 0, 0), (0, 0, 140), (0, 150, 220), (0, 220, 80), (250, 230, 0), (255, 60, 0), (255, 255, 255)]


def palette(f):
    f = max(0.0, min(1.0, f)) * (len(STOPS) - 1)
    k = min(int(f), len(STOPS) - 2)
    t = f - k
    a, b = STOPS[k], STOPS[k + 1]
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def spectrum(u):
    """Signal level 0..1 across the band (u in 0..1): a Wi-Fi block, a narrow
    carrier and a hop, over a gentle noise floor."""
    lvl = 0.12 + 0.03 * math.sin(u * 37.0) + 0.02 * math.sin(u * 91.0)
    lvl += 0.55 * max(0.0, 1 - abs(u - 0.36) / 0.13) ** 0.35  # 20 MHz-wide channel
    lvl += 0.70 * math.exp(-((u - 0.74) / 0.018) ** 2)        # narrow carrier
    lvl += 0.30 * math.exp(-((u - 0.88) / 0.025) ** 2)
    return min(lvl, 0.98)


def main(out_dir):
    random.seed(7)
    bg = (14, 18, 30)
    R = 104                               # corner radius
    top, mid, bot = 64, 262, 452          # spectrum 64..262, waterfall 262..452
    left, right = 52, S - 52
    # Waterfall rows: each row is the spectrum with burst noise; the channel
    # comes and goes like real traffic.
    rows = []
    for r in range(bot - mid):
        on = (r // 9) % 3 != 1
        hop = 0.15 + 0.7 * ((r // 23) * 0.37 % 1.0)
        row = []
        for x in range(right - left):
            u = x / (right - left)
            v = 0.10 + 0.04 * random.random()
            if on: v += 0.55 * max(0.0, 1 - abs(u - 0.36) / 0.13) ** 0.35
            v += 0.62 * math.exp(-((u - 0.74) / 0.018) ** 2)
            if (r % 23) < 5: v += 0.45 * math.exp(-((u - hop) / 0.02) ** 2)
            row.append(palette(min(v, 0.95) * 0.95))
        rows.append(row)

    px = bytearray(S * S * 3)
    for y in range(S):
        for x in range(S):
            # rounded-square mask with 1 px antialiasing
            dx = max(R - x, x - (S - 1 - R), 0)
            dy = max(R - y, y - (S - 1 - R), 0)
            d = math.hypot(dx, dy) - R
            alpha = max(0.0, min(1.0, 0.5 - d))
            c = bg
            if left <= x < right and top <= y < bot:
                u = (x - left) / (right - left)
                if y >= mid:
                    c = rows[y - mid][x - left]
                else:
                    h = spectrum(u)
                    yl = mid - 8 - h * (mid - top - 24)
                    if y > yl + 7:
                        c = (0, 78, 56)                       # fill
                    elif y >= yl - 7:
                        c = (90, 255, 140)                    # trace, 14 px thick
                    elif abs(y - (mid - 8 - 0.5 * (mid - top - 24))) < 2 or abs(u - 0.5) < 0.004:
                        c = (34, 44, 58)                      # grid line and centre marker
            c = tuple(int(c[i] * alpha + 255 * (1 - alpha)) for i in range(3))  # white outside
            px[(y * S + x) * 3:(y * S + x) * 3 + 3] = bytes(c)
    os.makedirs(out_dir, exist_ok=True)
    png(os.path.join(out_dir, "icon.png"), S, S, px)

    # 64x64 preview (box filter), the size Keira shows the icon at
    n = S // 64
    small = bytearray(64 * 64 * 3)
    for y in range(64):
        for x in range(64):
            for ch in range(3):
                s = 0
                for yy in range(n):
                    for xx in range(n):
                        s += px[((y * n + yy) * S + x * n + xx) * 3 + ch]
                small[(y * 64 + x) * 3 + ch] = s // (n * n)
    png(os.path.join(out_dir, "icon-64.png"), 64, 64, small)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "catalog")
