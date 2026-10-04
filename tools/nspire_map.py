#!/usr/bin/env python3
"""Converts an Xbox campaign map for the TI-Nspire port (port/nspire/README.md).

    python3 tools/nspire_map.py <maps/b30.map> <b30.map.tns>

The calculator cannot hold a map's tag data and structure BSP (about 22 MB)
at the tag cache's addresses, so the port pages them in from compressed
blocks (port/nspire/src/nspire_paging.c). This tool decompresses the Xbox
map (a 2 KB header, then zlib), finds the tag data and the BSPs the scenario
lists, and writes each as 16 KB blocks aligned to the addresses the game
loads it at, each deflated on its own (raw deflate, so the pager can inflate
any one).

Every bitmap is made small: its mipmap nearest 64x64 (32x32 for a cube map's
faces) is decoded and stored as a swizzled A4R4G4B4 level in the file's
pixel data, a 2D bitmap's followed by every smaller level down to one texel
(the calculator picks one for each triangle), and its header in the tag
data is rewritten to say so (format, size, mipmaps, the pixels' place in
this file). The calculator draws
320x240 and holds 2 MB of textures (halo_port_capacity.h). Sound data is
left out; the loader (port/nspire/game/cache_files_nspire.c) answers reads
of it with zeros.

The file (little endian):

    header       'hnsp', version, block size, region count, the pixel
                 data's offset and size, the Xbox map's 2 KB cache header
    regions      per region: kind (0 tag data, 1 structure BSP), the offset
                 and size of its data in the Xbox map, the address it loads
                 at, its first block's address, its block count, and the
                 file offsets of its block sizes and block data
    pixel data   the bitmaps' pixels, which the bitmap headers point at
    per region   the compressed size of each block, then the blocks

The ".tns" name lets the calculator's link software send the file.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"psnh"  # 'hnsp' read as a little-endian word
VERSION = 2
BLOCK_SIZE = 0x4000
CACHE_HEADER_SIZE = 0x800
TAG_CACHE_BASE_ADDRESS = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
TAG_INSTANCE_SIZE = 0x20
# struct scenario: its structure_bsps tag block (count, address, definition)
SCENARIO_STRUCTURE_BSPS_OFFSET = 0x5A4
# struct scenario_structure_bsp_reference: file offset, size, address
STRUCTURE_BSP_REFERENCE_SIZE = 0x20

REGION_TAG_DATA = 0
REGION_STRUCTURE_BSP = 1
REGION_ENTRY = struct.Struct("<8I")
HEADER = struct.Struct("<4s5I")


def decompress_map(data: bytes) -> bytes:
    """the whole map as the Xbox's cache partition holds it"""
    header = data[:CACHE_HEADER_SIZE]
    if header[:4] != b"daeh":
        sys.exit("not an Xbox cache file (no 'head' signature)")
    file_length = struct.unpack_from("<i", header, 8)[0]
    body = zlib.decompress(data[CACHE_HEADER_SIZE:])
    result = header + body
    if len(body) == file_length:
        # (a stream that includes its own copy of the header)
        result = body
    if len(result) != file_length:
        sys.exit(f"decompressed to {len(result)} bytes, the header says {file_length}")
    return result


def regions_of(cache: bytes):
    """(kind, file offset, size, load address) of the tag data and every BSP"""
    tag_data_offset, tag_data_size = struct.unpack_from("<ii", cache, 0x10)
    tags = cache[tag_data_offset:tag_data_offset + tag_data_size]

    def at(address: int, size: int) -> bytes:
        offset = address - TAG_CACHE_BASE_ADDRESS
        if offset < 0 or offset + size > len(tags):
            sys.exit(f"address {address:#x} is outside the tag data")
        return tags[offset:offset + size]

    instances, scenario_index = struct.unpack_from("<Ii", tags, 0)
    scenario = struct.unpack_from("<I", at(instances + (scenario_index & 0xFFFF) * TAG_INSTANCE_SIZE + 0x14, 4))[0]
    count, address = struct.unpack_from("<iI", at(scenario + SCENARIO_STRUCTURE_BSPS_OFFSET, 8))
    regions = [(REGION_TAG_DATA, tag_data_offset, tag_data_size, TAG_CACHE_BASE_ADDRESS)]
    for index in range(count):
        file_offset, size, base = struct.unpack_from("<III", at(address + index * STRUCTURE_BSP_REFERENCE_SIZE, 12))
        regions.append((REGION_STRUCTURE_BSP, file_offset, size, base))
    return regions


def blocks_of(data: bytes, address: int):
    """the first block's address and each 16 KB block's bytes, the data
    placed at address and zeros around it"""
    first = address & ~(BLOCK_SIZE - 1)
    end = (address + len(data) + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1)
    padded = bytes(address - first) + data + bytes(end - address - len(data))
    return first, [padded[offset:offset + BLOCK_SIZE] for offset in range(0, len(padded), BLOCK_SIZE)]


def deflate(block: bytes) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS, 9)
    return compressor.compress(block) + compressor.flush()


# ---------- bitmaps

TAG_INSTANCE_GROUP_BITMAP = b"mtib"  # 'bitm' as stored
BITMAP_GROUP_BITMAPS_OFFSET = 0x60  # struct bitmap_group: its bitmaps tag block
BITMAP_DATA_SIZE = 0x30  # struct bitmap_data (source/bitmaps/bitmap_group.h)
BITMAP_DATA = struct.Struct("<4s6h2hhhii")

BITMAP_TYPE_2D, BITMAP_TYPE_3D, BITMAP_TYPE_CUBE_MAP = 0, 1, 2
# the game's formats (source/cache/xbox_texture_cache.c)
FORMAT_A8, FORMAT_Y8, FORMAT_AY8, FORMAT_A8Y8 = 0, 1, 2, 3
FORMAT_R5G6B5, FORMAT_A1R5G5B5, FORMAT_A4R4G4B4 = 6, 8, 9
FORMAT_X8R8G8B8, FORMAT_A8R8G8B8 = 10, 11
FORMAT_DXT1, FORMAT_DXT3, FORMAT_DXT5, FORMAT_P8_BUMP = 14, 15, 16, 17
BYTES_PER_PIXEL = {
    FORMAT_A8: 1, FORMAT_Y8: 1, FORMAT_AY8: 1, FORMAT_A8Y8: 2, FORMAT_R5G6B5: 2, FORMAT_A1R5G5B5: 2,
    FORMAT_A4R4G4B4: 2, FORMAT_X8R8G8B8: 4, FORMAT_A8R8G8B8: 4, FORMAT_P8_BUMP: 1,
}
DXT_BLOCK_BYTES = {FORMAT_DXT1: 8, FORMAT_DXT3: 16, FORMAT_DXT5: 16}
# flags: power-of-two dimensions, swizzled, cached
CONVERTED_FLAGS = 0x01 | 0x08 | 0x80
CUBE_FACE_ALIGNMENT = 128
MAXIMUM_SIZE = {BITMAP_TYPE_2D: 64, BITMAP_TYPE_CUBE_MAP: 32, BITMAP_TYPE_3D: 4}


def level_bytes(format_: int, width: int, height: int, depth: int) -> int:
    if format_ in DXT_BLOCK_BYTES:
        return ((width + 3) // 4) * ((height + 3) // 4) * DXT_BLOCK_BYTES[format_] * depth
    return width * height * depth * BYTES_PER_PIXEL.get(format_, 4)


def swizzle_masks(width: int, height: int, depth: int = 1):
    """the Xbox's texel order: the bits of x, y and z interleaved while each
    lasts (port/linux/src/xbox_textures.c)"""
    masks = [0, 0, 0]
    bit = mask_bit = 1
    while True:
        done = True
        for axis, size in enumerate((width, height, depth)):
            if bit < size:
                masks[axis] |= mask_bit
                mask_bit <<= 1
                done = False
        bit <<= 1
        if done:
            return masks


def spread(mask: int, value: int) -> int:
    result, bit = 0, 1
    while value and bit <= mask:
        if mask & bit:
            if value & 1:
                result |= bit
            value >>= 1
        bit <<= 1
    return result


def expand(value: int, bits: int) -> int:
    return (value * 255 + (1 << bits) // 2 - 1) // ((1 << bits) - 1) if bits else 255


def decode_texel(format_: int, data: bytes, offset: int):
    """(a, r, g, b), each 0-255"""
    if format_ == FORMAT_A8:
        return (data[offset], 255, 255, 255)
    if format_ == FORMAT_Y8:
        return (255, data[offset], data[offset], data[offset])
    if format_ == FORMAT_AY8:
        return (data[offset],) * 4
    if format_ == FORMAT_P8_BUMP:
        # a bump map's index into the engine's own palette: a flat normal
        return (255, 128, 128, 255)
    if format_ == FORMAT_A8Y8:
        return (data[offset + 1], data[offset], data[offset], data[offset])
    if format_ in (FORMAT_R5G6B5, FORMAT_A1R5G5B5, FORMAT_A4R4G4B4):
        v = data[offset] | data[offset + 1] << 8
        if format_ == FORMAT_R5G6B5:
            return (255, expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5))
        if format_ == FORMAT_A1R5G5B5:
            return (255 if v & 0x8000 else 0, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5))
        return (expand(v >> 12, 4), expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4))
    b, g, r, a = data[offset:offset + 4]
    return (255 if format_ == FORMAT_X8R8G8B8 else a, r, g, b)


def decode_dxt(format_: int, data: bytes, width: int, height: int):
    """rows of (a, r, g, b)"""
    pixels = [[(0, 0, 0, 0)] * width for _ in range(height)]
    block_bytes = DXT_BLOCK_BYTES[format_]
    blocks_x = (width + 3) // 4
    for by in range((height + 3) // 4):
        for bx in range(blocks_x):
            block = data[(by * blocks_x + bx) * block_bytes:(by * blocks_x + bx + 1) * block_bytes]
            colors = block[-8:]
            c0, c1, bits = struct.unpack_from("<HHI", colors)

            def rgb(v):
                return (expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5))
            p0, p1 = rgb(c0), rgb(c1)
            if c0 > c1 or format_ != FORMAT_DXT1:
                palette = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                           tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
                transparent = -1
            else:
                palette = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
                transparent = 3
            alphas = [255] * 16
            if format_ == FORMAT_DXT3:
                alphas = [expand((block[i // 2] >> ((i & 1) * 4)) & 15, 4) for i in range(16)]
            elif format_ == FORMAT_DXT5:
                a0, a1 = block[0], block[1]
                abits = int.from_bytes(block[2:8], "little")
                values = [a0, a1] + ([((8 - i) * a0 + (i - 1) * a1) // 7 for i in range(2, 8)] if a0 > a1 else
                                     [((6 - i) * a0 + (i - 1) * a1) // 5 for i in range(2, 6)] + [0, 255])
                alphas = [values[(abits >> (3 * i)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < width and y < height:
                    index = (bits >> (2 * i)) & 3
                    alpha = 0 if index == transparent else alphas[i]
                    pixels[y][x] = (alpha,) + palette[index]
    return pixels


def decode_level(format_: int, data: bytes, width: int, height: int, swizzled: bool):
    if format_ in DXT_BLOCK_BYTES:
        return decode_dxt(format_, data, width, height)
    size = BYTES_PER_PIXEL.get(format_, 4)
    masks = swizzle_masks(width, height)
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            index = (spread(masks[0], x) | spread(masks[1], y)) if swizzled else y * width + x
            row.append(decode_texel(format_, data, index * size))
        rows.append(row)
    return rows


def shrink(pixels, width: int, height: int):
    """a box filter to the size given"""
    source_height, source_width = len(pixels), len(pixels[0])
    step_x, step_y = source_width // width, source_height // height
    result = []
    for y in range(height):
        row = []
        for x in range(width):
            total = [0, 0, 0, 0]
            for yy in range(y * step_y, (y + 1) * step_y):
                for xx in range(x * step_x, (x + 1) * step_x):
                    for c in range(4):
                        total[c] += pixels[yy][xx][c]
            count = step_x * step_y
            row.append(tuple(t // count for t in total))
        result.append(row)
    return result


def encode_a4r4g4b4(pixels, width: int, height: int) -> bytes:
    """one swizzled A4R4G4B4 level"""
    out = bytearray(width * height * 2)
    masks = swizzle_masks(width, height)
    for y in range(height):
        for x in range(width):
            a, r, g, b = pixels[y][x]
            v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)
            index = spread(masks[0], x) | spread(masks[1], y)
            struct.pack_into("<H", out, index * 2, v)
    return bytes(out)


def converted_size(type_: int, width: int, height: int):
    """the level of the bitmap to keep (the first no larger than the
    calculator's size) and its dimensions"""
    maximum = MAXIMUM_SIZE[type_]
    level = 0
    while max(width >> level, height >> level) > maximum:
        level += 1
    return level, max(width >> level, 1), max(height >> level, 1)


def convert_bitmap(cache: bytes, entry) -> tuple:
    """(pixels, width, height, depth) of one bitmap, converted"""
    _, width, height, depth, type_, format_, flags, _, _, mipmaps, _, offset, size = entry
    swizzled = bool(flags & 0x08)
    if type_ == BITMAP_TYPE_3D:
        # (fog and noise volumes: not drawn on the calculator) a grey cube
        side = 4
        return encode_a4r4g4b4([[(255, 128, 128, 128)] * side * side] * side, side * side, side)[:side ** 3 * 2], \
            side, side, side, 0
    level, new_width, new_height = converted_size(type_, width, height)
    level = min(level, mipmaps)
    if level < converted_size(type_, width, height)[0]:
        # not enough mipmaps: shrink the smallest there is
        new_width, new_height = converted_size(type_, width >> level, height >> level)[1:]
    level_width, level_height = max(width >> level, 1), max(height >> level, 1)
    face_bytes = sum(level_bytes(format_, max(width >> l, 1), max(height >> l, 1), 1) for l in range(mipmaps + 1))
    if type_ == BITMAP_TYPE_CUBE_MAP:
        face_bytes = (face_bytes + CUBE_FACE_ALIGNMENT - 1) // CUBE_FACE_ALIGNMENT * CUBE_FACE_ALIGNMENT
    level_offset = sum(level_bytes(format_, max(width >> l, 1), max(height >> l, 1), 1) for l in range(level))
    faces = 6 if type_ == BITMAP_TYPE_CUBE_MAP else 1
    out = bytearray()
    extra_levels = 0
    for face in range(faces):
        start = offset + face * face_bytes + level_offset
        data = cache[start:start + level_bytes(format_, level_width, level_height, 1)]
        pixels = decode_level(format_, data, level_width, level_height, swizzled)
        if (level_width, level_height) != (new_width, new_height):
            pixels = shrink(pixels, new_width, new_height)
        face_pixels = encode_a4r4g4b4(pixels, new_width, new_height)
        if faces == 1:
            # 2D textures get every smaller level down to 1 texel, packed
            # after the first as the Xbox lays them out: the renderer picks
            # one for each triangle, so far surfaces are not noise
            width_, height_ = new_width, new_height
            while max(width_, height_) > 1:
                width_, height_ = max(width_ // 2, 1), max(height_ // 2, 1)
                pixels = shrink(pixels, width_, height_)
                face_pixels += encode_a4r4g4b4(pixels, width_, height_)
                extra_levels += 1
        else:
            face_pixels += bytes(-len(face_pixels) % CUBE_FACE_ALIGNMENT)
        out += face_pixels
    return bytes(out), new_width, new_height, 1, extra_levels


# (ui.map) the bitmaps the calculator's cut-down main menu shows (its own
# screen, the difficulty screen, the shared pieces), kept whole
LARGE_BITMAP_PREFIXES = ("ui\\shell\\main_menu\\", "ui\\shell\\bitmaps\\")
LARGE_BITMAP_SIZE = 256
# what the cut-down menu never shows, kept small
UNSHOWN_MENU_BITMAPS = ("postgame_carnage_report", "xdemos_wait", "spinner_list_3_wide_item_background",
                        "split_screen_bkd")


def is_menu_bitmap(name: str) -> bool:
    if any(name.endswith("\\" + unshown) for unshown in UNSHOWN_MENU_BITMAPS):
        return False
    if name.startswith("ui\\shell\\bitmaps\\") or name.startswith("ui\\shell\\main_menu\\difficulty_select\\"):
        return True
    return name.startswith("ui\\shell\\main_menu\\") and "\\" not in name[len("ui\\shell\\main_menu\\"):]


def shift_between(original: int, converted: int):
    """how many halvings make original into converted (None if none do)"""
    shift = 0
    while (original >> shift) > converted and shift < 15:
        shift += 1
    return shift if max(original >> shift, 1) == converted else None


def convert_bitmaps(cache: bytearray, pixel_data_offset: int, large_menus: bool = False) -> bytes:
    """rewrites every bitmap header in the tag data (inside cache) for the
    small copies it returns, which the file will hold from pixel_data_offset"""
    tag_data_offset, tag_data_size = struct.unpack_from("<ii", cache, 0x10)

    def address_to_offset(address: int) -> int:
        return tag_data_offset + address - TAG_CACHE_BASE_ADDRESS

    instances, _, _, count = struct.unpack_from("<IiIi", cache, tag_data_offset)
    pixel_data = bytearray()
    converted = 0
    for index in range(count):
        group, _, _, _, name_address, address, _, _ = struct.unpack_from(
            "<4sIIIIIII", cache, address_to_offset(instances + index * TAG_INSTANCE_SIZE))
        if group != TAG_INSTANCE_GROUP_BITMAP or not (TAG_CACHE_BASE_ADDRESS <= address <
                                                       TAG_CACHE_BASE_ADDRESS + tag_data_size):
            continue
        bitmap_count, bitmaps = struct.unpack_from("<iI", cache, address_to_offset(address + BITMAP_GROUP_BITMAPS_OFFSET))
        name_offset = address_to_offset(name_address)
        name = bytes(cache[name_offset:cache.index(b"\0", name_offset)]).decode("latin-1").lower()
        small = MAXIMUM_SIZE[BITMAP_TYPE_2D]
        if large_menus and is_menu_bitmap(name):
            MAXIMUM_SIZE[BITMAP_TYPE_2D] = LARGE_BITMAP_SIZE
        for bitmap_index in range(bitmap_count):
            entry_offset = address_to_offset(bitmaps + bitmap_index * BITMAP_DATA_SIZE)
            entry = BITMAP_DATA.unpack_from(cache, entry_offset)
            pixels, width, height, depth, extra_levels = convert_bitmap(cache, entry)
            new_offset = pixel_data_offset + len(pixel_data)
            pixel_data += pixels + bytes(-len(pixels) % 16)
            # width, height, depth, format, flags, mipmaps, pixels offset and
            # size. A 2D bitmap or cube map keeps its width and height, which
            # the game lays its interface out by (a smaller one would be
            # drawn smaller, from its corner); how many times each was halved
            # goes in the pad after the mipmap count, 0x5A in its high byte,
            # for the texture made of it (source/cache/xbox_texture_cache.c)
            original_width, original_height = entry[1], entry[2]
            shift_x, shift_y = shift_between(original_width, width), shift_between(original_height, height)
            if entry[4] != BITMAP_TYPE_3D and shift_x is not None and shift_y is not None:
                struct.pack_into("<H", cache, entry_offset + 0x16, 0x5A00 | shift_x | shift_y << 4)
            else:
                struct.pack_into("<3h", cache, entry_offset + 4, width, height, depth)
                struct.pack_into("<H", cache, entry_offset + 0x16, 0)
            struct.pack_into("<hH", cache, entry_offset + 0xC, FORMAT_A4R4G4B4, CONVERTED_FLAGS | (entry[6] & 0x80))
            struct.pack_into("<h", cache, entry_offset + 0x14, extra_levels)
            struct.pack_into("<ii", cache, entry_offset + 0x18, new_offset, len(pixels))
            converted += 1
        MAXIMUM_SIZE[BITMAP_TYPE_2D] = small
    print(f"bitmaps: {converted} made small, {len(pixel_data) / 1e6:.2f} MB of pixels")
    return bytes(pixel_data)


def check_blocks_apart(regions) -> None:
    """a BSP's blocks replace whatever blocks are there when it loads, so none
    may share a block with the tag data"""
    tag_kind, _, tag_size, tag_address = regions[0]
    tag_last_block = (tag_address + tag_size - 1) // BLOCK_SIZE
    for kind, _, size, address in regions[1:]:
        if address // BLOCK_SIZE <= tag_last_block:
            sys.exit(f"a BSP at {address:#x} shares a 16 KB block with the tag data")
        if address + size > TAG_CACHE_BASE_ADDRESS + TAG_CACHE_SIZE:
            sys.exit(f"a BSP at {address:#x} ends past the tag cache")


def convert(source: Path, destination: Path, large_menus: bool = False) -> None:
    cache = bytearray(decompress_map(source.read_bytes()))
    regions = regions_of(cache)
    check_blocks_apart(regions)

    # the pixel data comes right after the region table, so its place is
    # known before the tag data that points at it is compressed
    table_offset = HEADER.size + CACHE_HEADER_SIZE
    pixel_data_offset = table_offset + REGION_ENTRY.size * len(regions)
    pixel_data_offset += -pixel_data_offset % 16
    pixel_data = convert_bitmaps(cache, pixel_data_offset, large_menus)

    out = bytearray()
    out += HEADER.pack(MAGIC, VERSION, BLOCK_SIZE, len(regions), pixel_data_offset, len(pixel_data))
    out += cache[:CACHE_HEADER_SIZE]
    out += bytes(pixel_data_offset - len(out))
    out += pixel_data
    out += bytes(-len(out) % 4)
    for index, (kind, offset, size, address) in enumerate(regions):
        first, blocks = blocks_of(cache[offset:offset + size], address)
        compressed = [deflate(block) for block in blocks]
        sizes_offset = len(out)
        out += struct.pack(f"<{len(compressed)}I", *(len(block) for block in compressed))
        data_offset = len(out)
        for block in compressed:
            out += block
        # (blocks read with one read each; keep words aligned)
        out += bytes(-len(out) % 4)
        REGION_ENTRY.pack_into(out, table_offset + index * REGION_ENTRY.size,
                               kind, offset, size, address, first, len(compressed), sizes_offset, data_offset)
        name = "tag data" if kind == REGION_TAG_DATA else "structure BSP"
        print(f"{name} at {address:#010x}: {size / 1e6:.2f} MB -> "
              f"{sum(len(block) for block in compressed) / 1e6:.2f} MB in {len(compressed)} blocks")
    destination.write_bytes(out)
    print(f"wrote {destination} ({len(out) / 1e6:.2f} MB)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source", type=Path, help="an Xbox map, maps/b30.map")
    parser.add_argument("destination", type=Path, help="the calculator's file, b30.map.tns")
    parser.add_argument("--maximum-size", type=int, default=64,
                        help="the largest a 2D bitmap is kept (64)")
    args = parser.parse_args()
    MAXIMUM_SIZE[BITMAP_TYPE_2D] = args.maximum_size
    # (ui.map: the main menu's own bitmaps larger, LARGE_BITMAP_PREFIXES)
    convert(args.source, args.destination, args.source.stem.lower() == "ui")


if __name__ == "__main__":
    main()
