#!/usr/bin/env python3
"""Save screenshots from ESP-SDR on Lilka as PNG files.

Connect the Lilka over USB while the SDR app runs, start this script, then
tap SELECT on the Lilka (a short tap; holding it returns to Keira). Every tap
saves the screen as lilka-sdr-<n>.png. Ctrl+C stops the script.

    python lilka_screenshot.py COM5            # Windows
    python lilka_screenshot.py /dev/ttyACM0    # Linux

Needs pyserial (pip install pyserial). The PNG writer is built in, so no
imaging library is required.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial


def png_bytes(width, height, rows):
    """rows: list of bytearrays of 8-bit RGB triplets."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def rgb565_row(hexline, width):
    data = bytes.fromhex(hexline)
    out = bytearray()
    for x in range(width):
        v = (data[2 * x] << 8) | data[2 * x + 1]  # big-endian RGB565 (the SPI wire order)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))
    return out


def open_port(name):
    while True:
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = name, 115200, 0.5
            s.dtr, s.rts = True, False  # native USB: the device only sends while DTR is set
            s.open()
            return s
        except serial.SerialException:
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("port", help="serial port of the Lilka, e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("-o", "--out", default=".", help="folder for the PNG files (default: current folder)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    n = 1
    while os.path.exists(os.path.join(args.out, f"lilka-sdr-{n}.png")):
        n += 1

    port = open_port(args.port)
    print(f"Listening on {args.port}. Tap SELECT on the Lilka to take a screenshot; Ctrl+C to stop.")
    width = height = 0
    rows = None
    buf = b""
    while True:
        try:
            chunk = port.read(8192)
        except serial.SerialException:
            port.close()
            port = open_port(args.port)  # the port re-enumerates when the Lilka resets
            continue
        if not chunk:
            continue
        buf += chunk
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = raw.decode("ascii", "replace").strip()
            if not line.startswith("LILKA-SHOT "):
                continue
            parts = line.split(" ")
            if parts[1] == "BEGIN":
                width, height = int(parts[2]), int(parts[3])
                rows = [None] * height
            elif parts[1] == "END" and rows is not None:
                missing = sum(r is None for r in rows)
                if missing:
                    print(f"screenshot incomplete ({missing} rows lost), skipped; tap SELECT again")
                else:
                    path = os.path.join(args.out, f"lilka-sdr-{n}.png")
                    with open(path, "wb") as f:
                        f.write(png_bytes(width, height, rows))
                    print(f"saved {path}")
                    n += 1
                rows = None
            elif rows is not None and len(parts) == 3:
                y = int(parts[1])
                if 0 <= y < height and len(parts[2]) == width * 4:
                    rows[y] = rgb565_row(parts[2], width)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
