#!/usr/bin/env python3
"""Convert PNG/JPG images to RGB565 .bin for the T-Deck image viewer.

Usage:
    python3 img2rgb565.py photo.jpg                    # -> photo.bin
    python3 img2rgb565.py img.png /run/media/andy/TDECK/images/

Requires Pillow:  pip install Pillow

Output format: raw little-endian RGB565, row-major, matching the on-device
image viewer (320x240 max) and the map tile format. Images larger than the
screen are scaled down to fit.
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

MAX_W, MAX_H = 320, 240


def convert(src, dst, max_w=MAX_W, max_h=MAX_H):
    im = Image.open(src)
    im = im.convert("RGB")
    im.thumbnail((max_w, max_h))
    w, h = im.size
    px = im.load()
    with open(dst, "wb") as f:
        for y in range(h):
            row = bytearray(w * 2)
            for x in range(w):
                r, g, b = px[x, y]
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                struct.pack_into("<H", row, x * 2, rgb565)
            f.write(row)
    print(f"{src} -> {dst} ({w}x{h})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="+", help="PNG/JPG file(s)")
    ap.add_argument("outdir", nargs="?", help="optional target directory, "
                    "e.g. an SD card /images folder")
    ap.add_argument("--size", default=f"{MAX_W}x{MAX_H}",
                    help="max dimensions WxH (default 320x240)")
    args = ap.parse_args()

    try:
        max_w, max_h = (int(v) for v in args.size.lower().split("x"))
    except ValueError:
        ap.error("--size must look like 320x240")

    for src in args.image:
        stem = os.path.splitext(os.path.basename(src))[0]
        if args.outdir:
            os.makedirs(args.outdir, exist_ok=True)
            dst = os.path.join(args.outdir, stem + ".bin")
        else:
            dst = os.path.splitext(src)[0] + ".bin"
        convert(src, dst, max_w, max_h)


if __name__ == "__main__":
    main()
