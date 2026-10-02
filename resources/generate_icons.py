"""Regenerate the original geometric application icons with Python + Pillow.

The checked-in ICO files are used by MSBuild; Pillow is only needed for editing.
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def draw_icon(name, size):
    # Draw each size independently with antialiasing and a transparent surround.
    scale = size * 4 / 256
    image = Image.new("RGBA", (size * 4, size * 4))
    draw = ImageDraw.Draw(image)

    def box(values):
        return tuple(round(v * scale) for v in values)

    def line(points, fill, width):
        draw.line([box(p) for p in points], fill=fill,
                  width=max(1, round(width * scale)), joint="curve")

    blue, teal, white = "#2563EB", "#087F8C", "#FFFFFF"
    draw.rounded_rectangle(box((8, 8, 248, 248)), radius=round(54 * scale),
                           fill=blue if name == "lfhookcfg" else teal)
    if name == "lfhookcfg":
        # A shield surrounding a location marker: the privacy configuration app.
        shield = [(128, 38), (204, 65), (198, 142), (179, 178),
                  (155, 200), (128, 218), (101, 200), (77, 178),
                  (58, 142), (52, 65), (128, 38)]
        line(shield, "#BFDBFE", 12)
        draw.ellipse(box((90, 78, 166, 154)), fill=white)
        draw.polygon([box(p) for p in [(96, 136), (160, 136), (128, 184)]], fill=white)
        draw.ellipse(box((115, 102, 141, 128)), fill=blue)
    else:
        # Crosshairs and a clear center point: the location viewer.
        draw.ellipse(box((65, 65, 191, 191)), outline=white, width=max(1, round(13 * scale)))
        for a, b in [((128, 40), (128, 82)), ((128, 174), (128, 216)),
                     ((40, 128), (82, 128)), ((174, 128), (216, 128))]:
            line([a, b], white, 13)
        draw.ellipse(box((107, 107, 149, 149)), fill="#99F6E4")
    return image.resize((size, size), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    for name in ("lfhookcfg", "locationdemo"):
        images = [draw_icon(name, size) for size in SIZES]
        images[-1].save(ROOT / f"{name}.ico", format="ICO",
                        sizes=[(size, size) for size in SIZES], append_images=images[:-1])
