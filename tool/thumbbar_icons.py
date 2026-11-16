#!/usr/bin/env python3
"""Generates the Windows taskbar thumbnail-toolbar icons.

Dev-only: the .ico outputs are committed and compiled into the exe via
windows/runner/Runner.rc. Re-run only when changing the glyph design.
Glyphs are drawn on a 16-unit design grid, supersampled 8x per size and
downscaled with LANCZOS so every embedded size is crisp, not resampled.
"""
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 28, 32, 36, 40, 48]
SS = 8  # supersample factor
OUT = Path(__file__).resolve().parent.parent / "windows" / "runner" / "resources"
COLORS = {"white": (255, 255, 255), "dark": (28, 28, 28)}

# Shapes on a 16x16 grid: ("poly", [(x, y), ...]) or ("rect", (x0, y0, x1, y1)).
NEXT = [("poly", [(2.5, 2.5), (2.5, 13.5), (10.5, 8.0)]),
        ("rect", (11.0, 2.5, 13.5, 13.5))]
GLYPHS = {
    "play":  [("poly", [(4.5, 2.5), (4.5, 13.5), (13.5, 8.0)])],
    "pause": [("rect", (3.5, 2.5, 6.5, 13.5)), ("rect", (9.5, 2.5, 12.5, 13.5))],
    "next":  NEXT,
    # Previous = horizontal mirror of Next.
    "prev":  [(k, [(16 - x, y) for x, y in v]) if k == "poly"
              else (k, (16 - v[2], v[1], 16 - v[0], v[3])) for k, v in NEXT],
}


def render(shapes, size, rgb):
    big = size * SS
    scale = big / 16.0
    # Transparent pixels carry the glyph colour so LANCZOS leaves no dark fringe.
    img = Image.new("RGBA", (big, big), rgb + (0,))
    d = ImageDraw.Draw(img)
    for kind, v in shapes:
        if kind == "poly":
            d.polygon([(x * scale, y * scale) for x, y in v], fill=rgb + (255,))
        else:
            x0, y0, x1, y1 = (c * scale for c in v)
            d.rounded_rectangle((x0, y0, x1, y1), radius=0.75 * scale, fill=rgb + (255,))
    return img.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, shapes in GLYPHS.items():
        for tone, rgb in COLORS.items():
            frames = [render(shapes, s, rgb) for s in SIZES]
            path = OUT / f"thumb_{name}_{tone}.ico"
            frames[-1].save(path, format="ICO",
                            sizes=[(s, s) for s in SIZES],
                            append_images=frames[:-1])
            got = sorted(Image.open(path).ico.sizes())
            assert got == [(s, s) for s in SIZES], (path, got)
            print("wrote", path.relative_to(OUT.parent.parent.parent), got[0], "..", got[-1])


if __name__ == "__main__":
    main()
