#!/usr/bin/env python3
"""
Download OpenStreetMap raster tiles and convert them to raw RGB565 for the
T-Deck Plus offline map app.

Output layout on the SD card:
    /map/z<zoom>/<x>/<y>.bin   (256x256 RGB565, 131072 bytes each)

Usage:
    python3 tools/download_tiles.py --sd /media/andy/SDCARD \
        --lat 47.6062 --lon -122.3321 --zooms 10-16 --radius-tiles 8

Requires: pip install pillow requests

Tile servers: this script defaults to the OSM standard style. Please respect
the OSM tile usage policy (valid user-agent, moderate rates); use your own
tile provider if you need bulk downloads (e.g. a self-hosted tile server,
openfreemap.org, or tile.openstreetmap.org within policy limits).
"""
import argparse
import math
import os
import sys
import time

try:
    import requests
    from PIL import Image
except ImportError:
    sys.exit("pip install pillow requests")

TILE_SERVERS = {
    "osm": "https://tile.openstreetmap.org",
    "carto": "https://basemaps.cartocdn.com/rastertiles/voyager",
    "carto-light": "https://basemaps.cartocdn.com/light_all",
    "opentopomap": "https://tile.opentopomap.org",
    "cyclosm": "https://a.tile-cyclosm.openstreetmap.fr/cyclosm",
}
TILE_PX = 256


def lon_to_tile_x(lon, zoom):
    return int((lon + 180.0) / 360.0 * (1 << zoom))


def lat_to_tile_y(lat, zoom):
    lat_r = math.radians(lat)
    return int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi)
               / 2.0 * (1 << zoom))


def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_png_to_rgb565(png_bytes):
    img = Image.open(__import__("io").BytesIO(png_bytes)).convert("RGB")
    img = img.resize((TILE_PX, TILE_PX))
    out = bytearray(TILE_PX * TILE_PX * 2)
    pixels = img.load()
    for y in range(TILE_PX):
        row = y * TILE_PX
        for x in range(TILE_PX):
            r, g, b = pixels[x, y]
            v = to_rgb565(r, g, b)
            i = (row + x) * 2
            # little-endian: low byte first (matches ESP32 read order)
            out[i] = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", required=True, help="SD card mount point, e.g. /media/$USER/SDCARD")
    ap.add_argument("--lat", type=float, required=True, help="center latitude")
    ap.add_argument("--lon", type=float, required=True, help="center longitude")
    ap.add_argument("--zooms", default="10-16", help="zoom range, e.g. 10-16 or 14")
    ap.add_argument("--radius-tiles", type=int, default=6,
                    help="tiles around center at each zoom (6 => ~13x13 area)")
    ap.add_argument("--tile-server", default="carto", choices=sorted(TILE_SERVERS),
                    help="tile provider: carto (default, no API key), osm, opentopomap, cyclosm")
    ap.add_argument("--contact", default=None,
                    help="your email or URL; required by OSM tile policy, appended to the User-Agent")
    args = ap.parse_args()

    tile_server = TILE_SERVERS[args.tile_server]
    ua = f"tdeck-notes-recorder/1.0 ({args.contact})" if args.contact else "tdeck-notes-recorder/1.0"
    HEADERS = {"User-Agent": ua}

    zooms = []
    if "-" in args.zooms:
        z0, z1 = args.zooms.split("-")
        zooms = list(range(int(z0), int(z1) + 1))
    else:
        zooms = [int(args.zooms)]

    session = requests.Session()
    session.headers.update(HEADERS)
    print(f"tile server: {args.tile_server} ({tile_server})")
    print(f"user-agent: {ua}")
    total, failed = 0, 0

    for z in zooms:
        cx = lon_to_tile_x(args.lon, z)
        cy = lat_to_tile_y(args.lat, z)
        r = args.radius_tiles
        zdir = os.path.join(args.sd, "map", f"z{z}")
        os.makedirs(zdir, exist_ok=True)
        for tx in range(cx - r, cx + r + 1):
            if tx < 0 or tx >= (1 << z):
                continue
            xdir = os.path.join(zdir, str(tx))
            os.makedirs(xdir, exist_ok=True)
            for ty in range(cy - r, cy + r + 1):
                if ty < 0 or ty >= (1 << z):
                    continue
                out_path = os.path.join(xdir, f"{ty}.bin")
                if os.path.exists(out_path) and os.path.getsize(out_path) == TILE_PX * TILE_PX * 2:
                    continue
                url = f"{tile_server}/{z}/{tx}/{ty}.png"
                try:
                    resp = session.get(url, timeout=20)
                    resp.raise_for_status()
                    data = convert_png_to_rgb565(resp.content)
                    with open(out_path, "wb") as f:
                        f.write(data)
                    total += 1
                    print(f"z{z} {tx},{ty} ok")
                except Exception as e:
                    failed += 1
                    print(f"z{z} {tx},{ty} FAILED: {e}")
                time.sleep(0.15)  # be polite to the tile server

    print(f"done: {total} tiles written, {failed} failed")
    print(f"SD layout: {os.path.join(args.sd, 'map')}/z<zoom>/<x>/<y>.bin")


if __name__ == "__main__":
    main()
