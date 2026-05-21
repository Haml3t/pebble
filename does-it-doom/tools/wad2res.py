#!/usr/bin/env python3
"""Extract shareware DOOM1.WAD assets, quantize to Pebble's 64-color
AARRGGBB-2222 palette, and emit Pebble resource files.

What it pulls from the WAD:
  - PLAYPAL[0]  -> palette LUT (256 bytes: Doom palette idx -> Pebble byte)
  - selected FLATs (64x64 raw indexed) used as wall textures
  - selected sprite frames (Doom picture format, decoded to indexed
    bitmap with a designated transparency-marker byte)

Output layout (each is a separate Pebble `raw` resource):
  resources/data/playpal.bin         256 B   doom_idx -> argb2222
  resources/data/tex_<NAME>.bin      4098 B  uint16(w),uint16(h)=64,64, data
  resources/data/spr_<NAME>.bin      4+N*M   uint16(w),uint16(h), data
                                              transparent byte = TRANSPARENT_IDX

Usage:
  wad2res.py [--wad path] [--out path]
"""
import argparse
import struct
import sys
from pathlib import Path

# Pebble64 = 4x4x4 RGB cube. Byte format AARRGGBB-2222: alpha=3 always
# opaque (top two bits = 0b11 = 0xC0). Each channel is 2 bits = 4 levels.
# Quantize Doom's 8-bit-per-channel RGB to this exact same mapping used
# in Glance's PebblePaletteEncoder.java (preserves visual parity).
def doom_rgb_to_pebble64(r: int, g: int, b: int) -> int:
    return (0xC0
        | (((r * 3 + 127) // 255) << 4)
        | (((g * 3 + 127) // 255) << 2)
        | (((b * 3 + 127) // 255) << 0))

# Reserve one Pebble color value as our transparency marker for sprites.
# 0x00 in AARRGGBB-2222 means alpha=0, fully transparent black — a value
# that should never be emitted for an opaque pixel by the quantizer.
TRANSPARENT_BYTE = 0x00


def remap_walls_to_warm_palette(data: bytes, header_size: int = 4) -> bytes:
    """Recolor every wall pixel into a 5-step warm-olive palette anchored
    at the player-requested colors (0xC4 dark olive / 0xD4 mid olive /
    0xE9 warm tan = lit). The 5 steps form a Manhattan-1 path in (R,G,B)
    space so consecutive brightness levels look like a smooth gradient,
    preserving the texture's vertical-panel detail from STARTAN3 etc.
        sum 0-1 -> 0xC4  (R=0 G=1 B=0, dark olive)
        sum 2-3 -> 0xD4  (R=1 G=1 B=0, olive)
        sum 4-5 -> 0xD8  (R=1 G=2 B=0, lighter olive)
        sum 6-7 -> 0xE8  (R=2 G=2 B=0, khaki / yellow)
        sum 8-9 -> 0xE9  (R=2 G=2 B=1, warm tan, lit accents)"""
    head = data[:header_size]
    pixels = bytearray(data[header_size:])
    for i, c in enumerate(pixels):
        if c == TRANSPARENT_BYTE:
            continue
        r = (c >> 4) & 0x3
        g = (c >> 2) & 0x3
        b = c & 0x3
        bright = r + g + b
        if bright <= 1:    pixels[i] = 0xC4
        elif bright <= 3:  pixels[i] = 0xD4
        elif bright <= 5:  pixels[i] = 0xD8
        elif bright <= 7:  pixels[i] = 0xE8
        else:              pixels[i] = 0xE9
    return head + bytes(pixels)


def darken_pebble64_pixels(data: bytes, drop_r: bool = True,
                           drop_g: bool = True,
                           header_size: int = 4) -> bytes:
    """Optionally knock R and/or G down by one quantum. Pebble64 has only
    4 levels per channel, so this is a coarse adjustment, but we have two
    knobs (drop_r, drop_g) which combined give a four-step ramp:
        both off  = original brightness   (sprites, HUD)
        drop_r    = R-only knock          (walls — "a little less dark")
        drop_g    = G-only knock          (unused; would look magenta-tinted)
        both on   = R+G knock             (floor + ceiling — atmospheric)
    B is left alone in all cases — Doom's palette skews warm, so dropping
    B would push everything toward cyan."""
    head = data[:header_size]
    pixels = bytearray(data[header_size:])
    for i, c in enumerate(pixels):
        if c == TRANSPARENT_BYTE:
            continue
        r = (c >> 4) & 0x3
        g = (c >> 2) & 0x3
        if drop_r and r > 0: r -= 1
        if drop_g and g > 0: g -= 1
        pixels[i] = (c & 0xC3) | (r << 4) | (g << 2)
    return head + bytes(pixels)


def read_wad_directory(wad_bytes: bytes):
    """Returns (magic, name -> (offset, size)) for all lumps."""
    magic, num_lumps, dir_offset = struct.unpack_from("<4sII", wad_bytes, 0)
    if magic not in (b"IWAD", b"PWAD"):
        raise SystemExit(f"not a wad: magic={magic!r}")
    lumps = {}
    # Doom lump directory: 30-day-since-1996 wisdom: 16 bytes per entry,
    # name is null-padded ASCII up to 8 bytes (no terminator if 8 long).
    for i in range(num_lumps):
        off = dir_offset + i * 16
        l_offset, l_size, l_name_raw = struct.unpack_from("<II8s", wad_bytes, off)
        l_name = l_name_raw.rstrip(b"\x00").decode("ascii", errors="replace")
        # F_START/F_END etc. share names with regular lumps in some wads.
        # For our purposes the first occurrence wins (the IWAD doesn't have
        # duplicates we care about).
        if l_name not in lumps:
            lumps[l_name] = (l_offset, l_size)
    return magic, lumps


def build_palette_lut(wad_bytes: bytes, lumps) -> bytes:
    """PLAYPAL is 14 256-entry palettes (256*3 bytes each = 768 each, 14
    palettes = 10752 bytes). We use palette 0 (the unpaletted look)."""
    offset, size = lumps["PLAYPAL"]
    if size < 768:
        raise SystemExit("PLAYPAL too small")
    pal = wad_bytes[offset : offset + 768]
    lut = bytearray(256)
    for i in range(256):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        lut[i] = doom_rgb_to_pebble64(r, g, b)
    # Force index 247 (commonly unused in Doom; cyan-ish #00FFFF region) to
    # the transparency byte — costs us one rarely-used palette slot for a
    # universal "this pixel is empty" marker readable by both walls (which
    # never use it) and sprites (which emit it for gaps).
    lut[247] = TRANSPARENT_BYTE
    return bytes(lut)


def extract_flat(wad_bytes: bytes, lumps, name: str, palette_lut: bytes) -> bytes:
    """A FLAT is a 64x64 raw indexed bitmap, 4096 bytes, palette-indexed."""
    offset, size = lumps[name]
    if size != 4096:
        raise SystemExit(f"flat {name} unexpected size {size}")
    indexed = wad_bytes[offset : offset + 4096]
    pixels = bytes(palette_lut[b] for b in indexed)
    # Resource format: u16 width, u16 height, then row-major pixel bytes.
    return struct.pack("<HH", 64, 64) + pixels


def decode_picture(wad_bytes: bytes, lump_offset: int, lump_size: int,
                   palette_lut: bytes) -> tuple:
    """Decode Doom's picture (patch/sprite) format into a flat indexed
    bitmap with `TRANSPARENT_BYTE` for empty pixels.

    Format:
        u16 width, u16 height, i16 left, i16 top,
        u32 col_offsets[width],
        then for each column:
            posts ::= post* 0xFF
            post  ::= u8 topdelta, u8 length, u8 unused,
                      u8 pixels[length], u8 unused
    """
    data = wad_bytes[lump_offset : lump_offset + lump_size]
    width, height, left, top = struct.unpack_from("<HHhh", data, 0)
    col_offsets = struct.unpack_from(f"<{width}I", data, 8)

    pixels = bytearray([TRANSPARENT_BYTE] * (width * height))
    for x in range(width):
        col_off = col_offsets[x]
        # Post chain
        while True:
            topdelta = data[col_off]; col_off += 1
            if topdelta == 0xFF:
                break
            length = data[col_off]; col_off += 1
            col_off += 1  # unused padding
            for i in range(length):
                y = topdelta + i
                if 0 <= y < height:
                    pixels[y * width + x] = palette_lut[data[col_off]]
                col_off += 1
            col_off += 1  # trailing unused padding
    return width, height, bytes(pixels)


def extract_picture(wad_bytes: bytes, lumps, name: str, palette_lut: bytes) -> bytes:
    offset, size = lumps[name]
    w, h, pixels = decode_picture(wad_bytes, offset, size, palette_lut)
    return struct.pack("<HH", w, h) + pixels


def parse_pnames(wad_bytes: bytes, lumps) -> list:
    """PNAMES: u32 count, then count*8-byte patch lump names."""
    offset, size = lumps["PNAMES"]
    count = struct.unpack_from("<I", wad_bytes, offset)[0]
    names = []
    for i in range(count):
        raw = wad_bytes[offset + 4 + i * 8 : offset + 4 + (i + 1) * 8]
        names.append(raw.rstrip(b"\x00").decode("ascii", errors="replace").upper())
    return names


def composite_texture(wad_bytes: bytes, lumps, name: str, palette_lut: bytes,
                      pnames: list) -> bytes:
    """A TEXTURE in Doom is composed of one or more patches stamped onto
    a width*height canvas. Patches are themselves picture-format lumps.
    Returns Pebble resource bytes: <u16 w><u16 h><pixels>."""
    # TEXTURE1: u32 count, then count*u32 offsets to per-texture records.
    # Each record: 8-byte name, 4B masked, 2B w, 2B h, 4B coldir, 2B patchcount,
    #             then patchcount*(2B originx, 2B originy, 2B patch_id, 2B step, 2B cmap).
    tex_offset, _ = lumps["TEXTURE1"]
    count = struct.unpack_from("<I", wad_bytes, tex_offset)[0]
    name_u = name.upper().encode("ascii")
    tex_record_offset = None
    for i in range(count):
        rec_off = struct.unpack_from("<I", wad_bytes, tex_offset + 4 + i * 4)[0]
        rec_name = wad_bytes[tex_offset + rec_off : tex_offset + rec_off + 8].rstrip(b"\x00")
        if rec_name == name_u:
            tex_record_offset = tex_offset + rec_off
            break
    if tex_record_offset is None:
        raise SystemExit("texture not in TEXTURE1: " + name)

    w, h = struct.unpack_from("<HH", wad_bytes, tex_record_offset + 12)
    patchcount = struct.unpack_from("<H", wad_bytes, tex_record_offset + 20)[0]

    canvas = bytearray([TRANSPARENT_BYTE] * (w * h))
    for p in range(patchcount):
        p_off = tex_record_offset + 22 + p * 10
        origin_x, origin_y, patch_id = struct.unpack_from("<hhH", wad_bytes, p_off)
        patch_name = pnames[patch_id]
        if patch_name not in lumps:
            continue
        lp_off, lp_sz = lumps[patch_name]
        pw, ph, ppx = decode_picture(wad_bytes, lp_off, lp_sz, palette_lut)
        for py in range(ph):
            yy = origin_y + py
            if yy < 0 or yy >= h:
                continue
            for px in range(pw):
                xx = origin_x + px
                if xx < 0 or xx >= w:
                    continue
                c = ppx[py * pw + px]
                if c != TRANSPARENT_BYTE:
                    canvas[yy * w + xx] = c

    return struct.pack("<HH", w, h) + bytes(canvas)


# Curated asset list. Names are case-sensitive 8-byte Doom lump names.
# Picks chosen for visual variety + the v1 raycaster's expected tilemap.
WALL_TEXTURES_TO_EXTRACT = [
    "STARTAN3",   # the famous E1M1 hangar wall (gray steel with vents)
    "BROWN1",     # brown brick wall — E1M1 corridor staple
    "SUPPORT2",   # support pillar — vertical metal strips
]
FLATS_TO_EXTRACT = [
    "FLOOR4_1",   # brown grid floor — Doom signature
    "CEIL3_5",    # dark ceiling tile
    "FLAT5_4",    # gray hangar-style floor (E1M1 staple)
    "NUKAGE1",    # green nukage (animated; we grab one frame)
    "STEP1",      # gray step
    "LITE3",      # bright wall
    "BROWN1",     # brown wall — E1M1 corridor staple (actually a texture
                  # composite, but Doom *also* ships a flat with this name
                  # in some IWADs; if missing we skip)
]
SPRITES_TO_EXTRACT = [
    "PISGA0",     # pistol (player's first weapon)
    "PISFA0",     # pistol firing flash
    "POSSA1",     # zombie standing, rotation 1 (front-facing)
    "POSSA2A8",   # zombie standing, rotation 2+8 (front-quarter)
    "TROOA1",     # imp standing front
    "PUFFA0",     # bullet puff (hit feedback)
    "STFST01",    # status bar face (full health, looking straight)
    # Big red HUD numerals (status bar health/ammo).
    "STTNUM0", "STTNUM1", "STTNUM2", "STTNUM3", "STTNUM4",
    "STTNUM5", "STTNUM6", "STTNUM7", "STTNUM8", "STTNUM9",
]


def emit_trig_lut(out_path: Path) -> int:
    """Precomputed sin/cos LUT. Pebble emery firmware's libc sinf/cosf
    crashes deterministically inside __ieee754_rem_pio2f (verified at PCs
    0x2030 and 0x2194 in production builds), so we cannot call libc trig
    on the watch — not even once. Bake the LUT here and load it as a
    resource at engine_init. 512 entries × 2 (sin+cos) × 4 bytes = 4096 B."""
    import math, struct
    n = 512
    data = bytearray()
    for i in range(n):
        a = 2.0 * math.pi * i / n
        data += struct.pack("<f", math.sin(a))
    for i in range(n):
        a = 2.0 * math.pi * i / n
        data += struct.pack("<f", math.cos(a))
    out_path.write_bytes(bytes(data))
    return len(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default="wad/doom1.wad", type=Path)
    ap.add_argument("--out", default="resources/data", type=Path)
    args = ap.parse_args()

    wad_bytes = args.wad.read_bytes()
    magic, lumps = read_wad_directory(wad_bytes)
    print(f"WAD: {args.wad}  magic={magic.decode()}  lumps={len(lumps)}")

    args.out.mkdir(parents=True, exist_ok=True)
    trig_bytes = emit_trig_lut(args.out / "trig_lut.bin")
    print(f"  trig_lut.bin             {trig_bytes:6d} B  (sin[512]+cos[512])")

    palette_lut = build_palette_lut(wad_bytes, lumps)
    (args.out / "playpal.bin").write_bytes(palette_lut)
    print(f"  playpal.bin                256 B  (Pebble64 LUT)")

    total_resource_bytes = 256
    extracted = []

    pnames = parse_pnames(wad_bytes, lumps)

    for name in WALL_TEXTURES_TO_EXTRACT:
        try:
            data = composite_texture(wad_bytes, lumps, name, palette_lut, pnames)
        except SystemExit as e:
            print(f"  SKIP wall {name}: {e}")
            continue
        # Walls are recolored to a 5-step warm-olive palette that
        # preserves STARTAN3's vertical-panel texture detail while
        # rotating the original dark-blue accents into warm tones.
        data = remap_walls_to_warm_palette(data)
        out_path = args.out / f"wall_{name.lower()}.bin"
        out_path.write_bytes(data)
        total_resource_bytes += len(data)
        extracted.append((out_path.name, len(data)))

    for name in FLATS_TO_EXTRACT:
        if name not in lumps:
            print(f"  SKIP flat {name} (not in WAD)")
            continue
        try:
            data = extract_flat(wad_bytes, lumps, name, palette_lut)
        except SystemExit as e:
            print(f"  SKIP flat {name}: {e}")
            continue
        data = darken_pebble64_pixels(data)
        out_path = args.out / f"tex_{name.lower()}.bin"
        out_path.write_bytes(data)
        total_resource_bytes += len(data)
        extracted.append((out_path.name, len(data)))

    for name in SPRITES_TO_EXTRACT:
        if name not in lumps:
            print(f"  SKIP sprite {name} (not in WAD)")
            continue
        try:
            data = extract_picture(wad_bytes, lumps, name, palette_lut)
        except (struct.error, IndexError, KeyError) as e:
            print(f"  SKIP sprite {name}: {e}")
            continue
        out_path = args.out / f"spr_{name.lower()}.bin"
        out_path.write_bytes(data)
        total_resource_bytes += len(data)
        extracted.append((out_path.name, len(data)))

    for name, size in extracted:
        print(f"  {name:30s}  {size:6d} B")
    print(f"  ---")
    print(f"  TOTAL EXTRACTED            {total_resource_bytes:6d} B "
          f"({100 * total_resource_bytes // (256 * 1024)}% of 256 KB cap)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
