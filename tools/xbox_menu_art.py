#!/usr/bin/env python3
"""Makes the original Xbox's copies of the PC menus' art.

The PC menus' pictures (port/assets/menus, tools/ce_menus.py) are drawn
for a desktop's resolution, up to 2048 texels a side: about 245 MB as
textures, more than the Xbox has. The Xbox draws its menus at 640x480, so
it takes each picture at the size its bitmap is drawn at (the frame's
width and height in the menus' bitmaps.xml, the size of the original
game's bitmap, always a power of two):
    python tools/xbox_menu_art.py
writes port/assets/menus/xbox/<the picture's path>, which the Xbox build
embeds in the originals' place (tools/embed_assets.py --menus-only --xbox)
and its renderer draws for the bitmap (port/xbox/src/d3d8_nv2a.c). Run it
again whenever the menus' pictures change (sources.json there records what
each copy was made from); --check only reports what is missing or out of
date, as the build does.

Needs Pillow. The pictures are resampled with premultiplied alpha, so
transparent texels' colors do not bleed into the edges.
"""

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MENU_ASSETS = ROOT / "port/assets/menus"
MENU_LIST = MENU_ASSETS / "menus.json"
XBOX_ART = MENU_ASSETS / "xbox"
# each copy's original's SHA-256 and the size it was made at, which tells a
# stale copy from a current one (file times do not survive a clone)
SOURCES = XBOX_ART / "sources.json"

FRAME = re.compile(r'<frame png="([^"]+)" width="(\d+)" height="(\d+)"')


def drawn_sizes() -> dict:
    """each picture's size as drawn: the largest of the frames that use it"""
    sizes: dict = {}
    for name in json.loads(MENU_LIST.read_text())["files"]:
        if not name.endswith(".xml"):
            continue
        for png, width, height in FRAME.findall((MENU_ASSETS / name).read_text()):
            old = sizes.get(png, (0, 0))
            sizes[png] = (max(old[0], int(width)), max(old[1], int(height)))
    return sizes


def fingerprint(png: str, size: tuple) -> str:
    return f"{hashlib.sha256((MENU_ASSETS / png).read_bytes()).hexdigest()} {size[0]}x{size[1]}"


def out_of_date(sizes: dict) -> list:
    """the pictures whose copies are missing or were made from another
    original or for another size (tools/embed_assets.py asks too)"""
    made = json.loads(SOURCES.read_text()) if SOURCES.is_file() else {}
    return [png for png in sorted(sizes)
            if not (XBOX_ART / png).is_file() or made.get(png) != fingerprint(png, sizes[png])]


def make(png: str, size: tuple) -> None:
    from PIL import Image

    with Image.open(MENU_ASSETS / png) as source:
        picture = source.convert("RGBA")
    if picture.size != size:
        picture = picture.convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
    output = XBOX_ART / png
    output.parent.mkdir(parents=True, exist_ok=True)
    # (8-bit RGBA, not interlaced: what port/linux/src/png_decode.c reads)
    picture.save(output, optimize=True)


def main() -> None:
    sizes = drawn_sizes()
    stale = out_of_date(sizes)
    if "--check" in sys.argv[1:]:
        for png in stale:
            print(f"port/assets/menus/xbox/{png} is missing or out of date")
        sys.exit(1 if stale else 0)
    for png in sorted(sizes):
        make(png, sizes[png])
    SOURCES.write_text(json.dumps({png: fingerprint(png, sizes[png]) for png in sorted(sizes)}, indent=1) + "\n")
    total = sum((XBOX_ART / png).stat().st_size for png in sizes)
    texels = sum(width * height for width, height in sizes.values())
    print(f"{len(sizes)} pictures, {total // 1024} KB of PNG, {texels * 4 // 1024} KB as textures")


if __name__ == "__main__":
    main()
