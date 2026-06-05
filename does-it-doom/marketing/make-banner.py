#!/usr/bin/env python3
"""Generate banner-image candidates for the Rebble listing.

Target ~640x320. Two designs:
  banner-A: hero screenshot scaled up, with title text overlaid at top
  banner-B: side-by-side, hero on left + title block on right
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
HERO = HERE / "hero-200x228.png"
ROOMC = HERE / "room-c-pool.png"

# Try a few common font locations; fall back to default if none found.
FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
]


def load_font(size: int):
    for p in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(p, size)
        except (OSError, IOError):
            continue
    return ImageFont.load_default()


def banner_a():
    """Hero center-scaled, title text top, byline bottom."""
    W, H = 640, 320
    out = Image.new("RGB", (W, H), (16, 0, 0))   # very dark red
    hero = Image.open(HERO).convert("RGB")
    # Scale hero to ~280px tall, fit width.
    target_h = 280
    sw, sh = hero.size
    scale = target_h / sh
    new_w = int(sw * scale)
    scaled = hero.resize((new_w, target_h), Image.NEAREST)
    x = (W - new_w) // 2
    y = (H - target_h) // 2 + 10
    out.paste(scaled, (x, y))

    draw = ImageDraw.Draw(out)
    title_font = load_font(56)
    title = "DOES IT DOOM?"
    bbox = draw.textbbox((0, 0), title, font=title_font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    tx = (W - tw) // 2
    ty = 8
    # Dark red drop shadow then bright red
    draw.text((tx + 2, ty + 2), title, font=title_font, fill=(80, 0, 0))
    draw.text((tx, ty), title, font=title_font, fill=(220, 0, 0))
    return out


def banner_b():
    """Two-column: hero screenshot left, title block right."""
    W, H = 640, 320
    out = Image.new("RGB", (W, H), (0, 0, 0))
    # Subtle red gradient on the right column
    draw_grad = ImageDraw.Draw(out)
    for i in range(W // 2, W):
        t = (i - W // 2) / (W // 2)
        r = int(20 + t * 40)
        draw_grad.line([(i, 0), (i, H)], fill=(r, 0, 0))

    hero = Image.open(HERO).convert("RGB")
    target_h = H - 20
    sw, sh = hero.size
    scale = target_h / sh
    new_w = int(sw * scale)
    scaled = hero.resize((new_w, target_h), Image.NEAREST)
    out.paste(scaled, (10, 10))

    draw = ImageDraw.Draw(out)
    title_font = load_font(48)
    sub_font = load_font(22)
    title_x = W // 2 + 20
    # Two-line title
    draw.text((title_x + 2, 50 + 2), "DOES IT", font=title_font, fill=(60, 0, 0))
    draw.text((title_x, 50), "DOES IT", font=title_font, fill=(220, 30, 30))
    draw.text((title_x + 2, 110 + 2), "DOOM?", font=title_font, fill=(60, 0, 0))
    draw.text((title_x, 110), "DOOM?", font=title_font, fill=(220, 30, 30))
    # Tagline
    draw.text((title_x, 190), "A Doom raycaster", font=sub_font, fill=(200, 200, 200))
    draw.text((title_x, 218), "for Pebble Time 2", font=sub_font, fill=(200, 200, 200))
    return out


def main():
    for name, fn in [("banner-A-stacked.png", banner_a),
                     ("banner-B-split.png", banner_b)]:
        img = fn()
        img.save(HERE / name)
        print(f"wrote {HERE / name}  ({img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
