#!/usr/bin/env python3
"""Embeds the high-res HUD textures (port/assets/hud, made by
tools/hud_assets.py), the menus' titles (port/assets/titles, made by
tools/title_assets.py), their controller button icons (port/assets/buttons,
made by tools/button_assets.py), the fonts the text is drawn with
(port/assets/fonts), the menus' files (port/assets/menus, made by
tools/ce_menus.py) and SMAA's shader and lookup textures
(port/third_party/smaa) in the game as C data:

    python tools/embed_assets.py OUTPUT.c

writes OUTPUT.c with each PNG and the bitmap it stands for (its tag, index
and checksum of its pixels, from port/assets/hud/layout.json,
port/assets/titles/titles.json and port/assets/buttons/buttons.json), the
fonts and their tags from port/assets/fonts/fonts.json, the menu files
listed in port/assets/menus/menus.json, and SMAA's shader and lookup
textures, as port/linux/src/hud_hires.h, text_hires.h, menu_files.h and
xgpu_post.c declare them. The builds generate it
(hud_assets_build, called by tools/linux_build.py, windows_build.py and
android_build.py), so the assets are the committed source and Android needs
no files beside its guest image.

The data are 32-bit words, not bytes: the Android build passes the guest's
assembly through tools/android_asm_convert.py, which rewrites identifiers in
operands and would garble a long .ascii string of binary data.
"""

import json
import struct
import sys
from pathlib import Path
from typing import Any, List

ROOT = Path(__file__).resolve().parent.parent
HUD_ASSETS = Path("port/assets/hud")
LAYOUT = HUD_ASSETS / "layout.json"
TITLE_ASSETS = Path("port/assets/titles")
TITLE_LIST = TITLE_ASSETS / "titles.json"
BUTTON_ASSETS = Path("port/assets/buttons")
BUTTON_LIST = BUTTON_ASSETS / "buttons.json"
FONT_ASSETS = Path("port/assets/fonts")
FONT_LIST = FONT_ASSETS / "fonts.json"
MENU_ASSETS = Path("port/assets/menus")
MENU_LIST = MENU_ASSETS / "menus.json"
# SMAA's files and the names port/linux/src/xgpu_post.c declares them by
SMAA_ASSETS = Path("port/third_party/smaa")
SMAA_FILES = (("SMAA.hlsl", "xgpu_smaa_shader"), ("area_tex.zlib", "xgpu_smaa_area_texture"),
              ("search_tex.zlib", "xgpu_smaa_search_texture"))
# the desktop windows' icon (tools/app_icon.py; port/linux/src/sdl_platform.c)
WINDOW_ICON = Path("port/assets/icon/opence-icon-256.png")
# the original Xbox's copies of the menus' pictures, at the size they are
# drawn (tools/xbox_menu_art.py)
XBOX_MENU_ART = MENU_ASSETS / "xbox"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def font_files() -> List[str]:
    """The font files fonts.json uses, each once."""
    if not (ROOT / FONT_LIST).is_file():
        return []
    fonts = json.loads((ROOT / FONT_LIST).read_text())["fonts"]
    return sorted({font["file"] for font in fonts})


def textures() -> List[tuple]:
    """The textures: each one's folder, its entry in its list, and whether
    it is the menus' (a title or a button icon, drawn with the high-res
    text)."""
    result = []
    for folder, listing, title in ((HUD_ASSETS, LAYOUT, False), (TITLE_ASSETS, TITLE_LIST, True),
                                   (BUTTON_ASSETS, BUTTON_LIST, True)):
        if (ROOT / listing).is_file():
            result += [(folder, asset, title) for asset in json.loads((ROOT / listing).read_text())["assets"]]
    return result


def menu_files() -> List[str]:
    """The menus' files menus.json lists, relative to their folder."""
    if not (ROOT / MENU_LIST).is_file():
        return []
    return json.loads((ROOT / MENU_LIST).read_text())["files"]


def smaa_files() -> List[tuple]:
    """SMAA's files that the checkout has, with their symbols."""
    return [(name, symbol) for name, symbol in SMAA_FILES if (ROOT / SMAA_ASSETS / name).is_file()]


def xbox_menu_inputs() -> List[Path]:
    """The files the original Xbox's menus are embedded from: the menus'
    files, with the Xbox's copies of their pictures (tools/xbox_menu_art.py)
    in place of the pictures."""
    return [MENU_LIST, XBOX_MENU_ART / "sources.json", *(MENU_ASSETS / name for name in menu_files()),
            *(XBOX_MENU_ART / name for name in menu_files() if name.endswith(".png"))]


def hud_asset_inputs() -> List[Path]:
    """The files the generated source is made from."""
    inputs = [listing for listing in (LAYOUT, TITLE_LIST, BUTTON_LIST, FONT_LIST, MENU_LIST)
              if (ROOT / listing).is_file()]
    return [*inputs, *(folder / f"{asset['name']}.png" for folder, asset, _ in textures()),
            *(FONT_ASSETS / name for name in font_files()), *(MENU_ASSETS / name for name in menu_files()),
            *(SMAA_ASSETS / name for name, _ in smaa_files()),
            *([WINDOW_ICON] if (ROOT / WINDOW_ICON).is_file() else [])]


def hud_configure_inputs() -> List[Path]:
    """What configure.py is run again for: the lists of assets (and the
    folders, for files added or removed), not each file, which a change of a
    list may rename or remove."""
    inputs = []
    for folder, listing in ((HUD_ASSETS, LAYOUT), (TITLE_ASSETS, TITLE_LIST), (BUTTON_ASSETS, BUTTON_LIST),
                            (FONT_ASSETS, FONT_LIST), (MENU_ASSETS, MENU_LIST)):
        if (ROOT / listing).is_file():
            inputs += [folder, listing]
    if (ROOT / SMAA_ASSETS).is_dir():
        inputs.append(SMAA_ASSETS)
    return inputs


def words(data: bytes) -> List[str]:
    """data as lines of 32-bit words (padded with zeros)."""
    padded = data + b"\0" * (-len(data) % 4)
    values = struct.unpack(f"<{len(padded) // 4}I", padded)
    return ["\t" + ", ".join(f"0x{word:08x}" for word in values[start:start + 8]) + ","
            for start in range(0, len(values), 8)]


def hud_assets_build(n: Any, prefix: str, output: Path) -> List[Path]:
    """Emits the rule that generates output; returns [output]. It is made
    whatever assets the checkout has (with none, its tables are empty), so
    that the symbols the platform layer refers to are always defined."""
    inputs = hud_asset_inputs()
    n.rule(
        name=f"{prefix}_embed_assets",
        command="$python tools/embed_assets.py $out",
        description=f"{prefix.upper()} EMBED $out",
    )
    n.build(outputs=output, rule=f"{prefix}_embed_assets", implicit=[Path("tools/embed_assets.py"), *inputs])
    return [output]


def png_size(data: bytes, name: str) -> tuple:
    """The width and height of an 8-bit RGBA, non-interlaced PNG (the only
    kind port/linux/src/hud_hires.c reads)."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        sys.exit(f"{name}: not a PNG")
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", data[16:29])
    if depth != 8 or colour != 6 or interlace != 0:
        sys.exit(f"{name}: must be 8-bit RGBA and not interlaced")
    return width, height


def embed_textures_and_fonts(lines: List[str]) -> None:
    """the high-res HUD's and titles' textures (hud_hires.h) and the fonts
    (text_hires.h)"""
    lines.append('#include "hud_hires.h"')
    lines.append("")
    table = []
    for index, (folder, asset, title) in enumerate(textures()):
        name = f"{asset['name']}.png"
        data = (ROOT / folder / name).read_bytes()
        width, height = png_size(data, name)
        scale = asset["scale"]
        if (width, height) != (asset["width"] * scale, asset["height"] * scale):
            sys.exit(f"{name}: {width}x{height}, not {scale}x its bitmap's {asset['width']}x{asset['height']}")
        lines.append(f"static const unsigned int asset{index}[] = {{")
        lines.extend(words(data))
        lines.append("};")
        lines.append("")
        tag = asset["tag"].replace("\\", "\\\\")
        coverage = int(any(cell["kind"] == "meter" for cell in asset.get("cells", [])))
        point_threshold = int(any(cell.get("thresholds") for cell in asset.get("cells", [])))
        # (the sequences whose sprites it stands for, a bit each of 32; 0: the whole bitmap)
        sequences = [sprite["sequence"] for sprite in asset.get("sprites", [])]
        if any(not 0 <= sequence < 32 for sequence in sequences):
            sys.exit(f"{name}: sprite sequences must be 0 to 31 (hud_hires.h: sprites)")
        sprites = sum(1 << sequence for sequence in sequences)
        table.append(f'\t{{ "{tag}", {asset["bitmap"]}, {width}, {height}, 0x{asset["crc"]:08x}u, {coverage}, '
                     f'{point_threshold}, {int(title)}, 0x{sprites:x}u, asset{index}, {len(data)} }},')
    lines.append("const struct hud_hires_embedded hud_hires_embedded[] =")
    lines.append("{")
    lines.extend(table)
    if not table:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int hud_hires_embedded_count = {len(table)};")
    lines.append("")
    # the fonts, and which draws each font tag (text_hires.h)
    lines.append('#include "text_hires.h"')
    lines.append("")
    files = font_files()
    for index, name in enumerate(files):
        lines.append(f"static const unsigned int font{index}[] = {{")
        lines.extend(words((ROOT / FONT_ASSETS / name).read_bytes()))
        lines.append("};")
        lines.append("")
    fonts = json.loads((ROOT / FONT_LIST).read_text())["fonts"] if files else []
    lines.append("const struct text_hires_embedded text_hires_embedded[] =")
    lines.append("{")
    for font in fonts:
        index = files.index(font["file"])
        tag = font["tag"].replace("\\", "\\\\")
        size = (ROOT / FONT_ASSETS / font["file"]).stat().st_size
        lines.append(f'\t{{ "{tag}", "{font["file"]}", font{index}, {size} }},')
    if not fonts:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int text_hires_embedded_count = {len(fonts)};")
    lines.append("")


def main() -> None:
    arguments = sys.argv[1:]
    # (the original Xbox leaves the high-res HUD and text out: the menus'
    # files alone, tools/xbox_build.py)
    menus_only = "--menus-only" in arguments
    # (and its menus' pictures are its own smaller copies, under the
    # originals' names)
    xbox = "--xbox" in arguments
    arguments = [argument for argument in arguments if argument not in ("--menus-only", "--xbox")]
    if len(arguments) != 1:
        sys.exit("usage: embed_assets.py [--menus-only [--xbox]] OUTPUT.c")
    lines = [
        "/* generated by tools/embed_assets.py from port/assets: do not edit */",
        "",
    ]
    if not menus_only:
        embed_textures_and_fonts(lines)
    # the menus' files (menu_files.h)
    lines.append('#include "menu_files.h"')
    lines.append("")
    menus = menu_files()
    sources = {name: ROOT / (XBOX_MENU_ART if xbox and name.endswith(".png") else MENU_ASSETS) / name
               for name in menus}
    if xbox:
        sys.path.insert(0, str(ROOT / "tools"))
        from xbox_menu_art import drawn_sizes, out_of_date
        stale = out_of_date(drawn_sizes())
        if stale:
            sys.exit(f"the Xbox's copy of {stale[0]} (and {len(stale) - 1} more) is missing or out of date: "
                     "run tools/xbox_menu_art.py")
    for index, name in enumerate(menus):
        data = sources[name].read_bytes()
        if name.endswith(".png"):
            png_size(data, name)
        lines.append(f"static const unsigned int menu{index}[] = {{")
        lines.extend(words(data))
        lines.append("};")
        lines.append("")
    lines.append("const struct menu_file_embedded menu_files_embedded[] =")
    lines.append("{")
    for index, name in enumerate(menus):
        size = sources[name].stat().st_size
        lines.append(f'\t{{ "{name}", menu{index}, {size} }},')
    if not menus:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int menu_files_embedded_count = {len(menus)};")
    # (and SMAA and the window icon, which the original Xbox has neither of)
    if not menus_only:
        lines.append("")
        # SMAA's shader, as text a GLSL compiler takes (ASCII, ending in a NUL),
        # and its lookup textures (xgpu_post.c); each of size 0 that the
        # checkout does not have. The OpenGL ES renderer has no SMAA.
        lines.append("#ifndef HALO_GLES")
        present = dict(smaa_files())
        for name, symbol in SMAA_FILES:
            data = (ROOT / SMAA_ASSETS / name).read_bytes() if name in present else b""
            if data and name.endswith(".hlsl"):
                data = bytes(byte if byte < 0x80 else 0x20 for byte in data) + b"\0"
            lines.append("")
            lines.append(f"const unsigned int {symbol}[] = {{")
            lines.extend(words(data) if data else ["\t0,"])
            lines.append("};")
            lines.append(f"const unsigned long {symbol}_size = {len(data)};")
        # the windows' icon, a PNG (of size 0 where the checkout has none)
        data = (ROOT / WINDOW_ICON).read_bytes() if (ROOT / WINDOW_ICON).is_file() else b""
        lines.append("")
        lines.append("const unsigned int platform_window_icon[] = {")
        lines.extend(words(data) if data else ["\t0,"])
        lines.append("};")
        lines.append(f"const unsigned long platform_window_icon_size = {len(data)};")
        lines.append("")
        lines.append("#endif")
    output = Path(arguments[0])
    output.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    if not output.is_file() or output.read_text() != text:
        output.write_text(text)


if __name__ == "__main__":
    main()
