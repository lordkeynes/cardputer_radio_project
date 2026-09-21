#!/usr/bin/env python3
"""Convert EPUB ebooks to plain text for the T-Deck ebook reader.

Usage:
    python3 epub2txt.py book.epub            # -> book.txt next to the epub
    python3 epub2txt.py book.epub -o out.txt
    python3 epub2txt.py *.epub /run/media/andy/TDECK/books/

The T-Deck reader displays raw UTF-8 .txt files from the /books directory
on the SD card. This tool unzips the EPUB (a zip of XHTML chapters), reads
the OPF manifest/spine to get chapter order, strips tags, and writes a
plain-text version with paragraph breaks preserved.

Only the Python standard library is required.
"""

import argparse
import html
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

CONTAINER_NS = "{urn:oasis:names:tc:opendocument:xmlns:container}"
OPF_NS = "{http://www.idpf.org/2007/opf}"
XHTML_BLOCK_TAGS = {
    "p", "div", "h1", "h2", "h3", "h4", "h5", "h6",
    "li", "blockquote", "br", "tr", "table",
}


def opf_path(z):
    container = z.read("META-INF/container.xml")
    root = ET.fromstring(container)
    rootfile = root.find(f"{CONTAINER_NS}rootfiles/{CONTAINER_NS}rootfile")
    return rootfile.get("full-path")


def spine_order(z, opf_name):
    """Return hrefs (relative to the OPF) in spine order."""
    opf = ET.fromstring(z.read(opf_name))
    base = opf_name.rsplit("/", 1)[0] + "/" if "/" in opf_name else ""
    manifest = {}
    for item in opf.iter(f"{OPF_NS}item"):
        manifest[item.get("id")] = item.get("href")
    order = []
    for itemref in opf.iter(f"{OPF_NS}itemref"):
        href = manifest.get(itemref.get("idref"))
        if href:
            order.append(base + href)
    return order


def strip_xhtml(data):
    """Very small XHTML -> text converter: drops head/scripts/styles,
    turns block tags into line breaks, unescapes entities."""
    data = data.decode("utf-8", errors="replace")
    data = re.sub(r"<\s*(head|script|style)\b.*?<\s*/\s*\1\s*>",
                  "", data, flags=re.I | re.S)
    data = re.sub(r"<\s*(?:\?xml|!DOCTYPE)[^>]*>", "", data, flags=re.I)
    for tag in XHTML_BLOCK_TAGS:
        data = re.sub(rf"<\s*{tag}\b[^>]*/?>", "\n", data, flags=re.I)
        data = re.sub(rf"<\s*/\s*{tag}\s*>", "\n", data, flags=re.I)
    data = re.sub(r"<[^>]+>", "", data)
    data = html.unescape(data)
    lines = [re.sub(r"[ \t]+", " ", ln).strip() for ln in data.splitlines()]
    out, blank = [], False
    for ln in lines:
        if ln:
            out.append(ln)
            blank = False
        elif not blank:
            out.append("")
            blank = True
    return "\n".join(out).strip() + "\n"


def epub_to_text(path):
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        try:
            order = spine_order(z, opf_path(z))
        except Exception as exc:
            raise SystemExit(f"{path}: could not parse EPUB structure: {exc}")
        parts = []
        for href in order:
            href = html.unescape(href)
            if href not in names:
                continue
            if not href.lower().endswith((".xhtml", ".html", ".htm")):
                continue
            text = strip_xhtml(z.read(href))
            if text.strip():
                parts.append(text)
        return "\n\n\n".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("epub", nargs="+", help="EPUB file(s)")
    ap.add_argument("-o", "--output", help="output .txt (single input only)")
    ap.add_argument("outdir", nargs="?", help="optional target directory, "
                    "e.g. an SD card /books folder")
    args = ap.parse_args()

    if args.output and len(args.epub) > 1:
        ap.error("-o/--output works with a single input file")

    import os
    for src in args.epub:
        text = epub_to_text(src)
        stem = os.path.splitext(os.path.basename(src))[0]
        if args.output:
            dst = args.output
        elif args.outdir:
            os.makedirs(args.outdir, exist_ok=True)
            dst = os.path.join(args.outdir, stem + ".txt")
        else:
            dst = os.path.splitext(src)[0] + ".txt"
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"{src} -> {dst} ({len(text)} chars)")


if __name__ == "__main__":
    sys.exit(main())
