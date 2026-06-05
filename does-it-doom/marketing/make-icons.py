#!/usr/bin/env python3
"""Generate menu-icon candidates from existing Pebble64 sprite resources.

Each sprite is AARRGGBB-2222 (one byte per pixel: 2 bits A,R,G,B). Channel value
0..3 expands to 0/85/170/255 in 8-bit RGB. Alpha 0 (TRANSPARENT_BYTE) is
treated as background.
"""

import struct
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

DATA_DIR = Path(__file__).parent.parent / "resources" / "data"
OUT_DIR = Path(__file__).parent
TRANSPARENT = 0x00


def decode_sprite(path: Path):
    raw = path.read_bytes()
    w, h = struct.unpack("<HH", raw[:4])
    pixels = raw[4:]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            b = pixels[y * w + x]
            if b == TRANSPARENT:
                continue
            a = ((b >> 6) & 3) * 85
            r = ((b >> 4) & 3) * 85
            g = ((b >> 2) & 3) * 85
            bl = (b & 3) * 85
            img.putpixel((x, y), (r, g, bl, a if a else 255))
    return img


def fit_to(src: Image.Image, size: int, bg: tuple, pad_ratio: float = 0.08):
    """Scale src to fit (size,size) keeping aspect, paste onto bg."""
    out = Image.new("RGBA", (size, size), bg)
    inner = int(size * (1 - 2 * pad_ratio))
    sw, sh = src.size
    scale = min(inner / sw, inner / sh)
    nw, nh = int(sw * scale), int(sh * scale)
    scaled = src.resize((nw, nh), Image.NEAREST)
    ox = (size - nw) // 2
    oy = (size - nh) // 2
    out.alpha_composite(scaled, (ox, oy))
    return out


def main():
    imp = decode_sprite(DATA_DIR / "spr_trooa1.bin")
    face = decode_sprite(DATA_DIR / "spr_stfst01.bin")

    candidates = [
        ("icon-A-imp.png", fit_to(imp, 144, (24, 0, 0, 255))),       # dark red
        ("icon-B-face.png", fit_to(face, 144, (40, 40, 40, 255))),   # steel gray
        ("icon-C-imp-black.png", fit_to(imp, 144, (0, 0, 0, 255))),  # pure black
        # Pebble's launcher caps menu icons at 25x25 — the actual build resource:
        ("menu-icon-25.png", fit_to(face, 25, (40, 40, 40, 255), pad_ratio=0.04)),
    ]
    for name, img in candidates:
        out = OUT_DIR / name
        img.save(out)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
