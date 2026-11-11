"""
Génère 3ds/source/assets.h à partir des images de design.

    python tools/make_assets.py [chemin/vers/Poppins-Bold.ttf]

- 3ds/assets/top.png    (400x240) : fond de l'écran du haut en attente
- 3ds/assets/bottom.png (320x240) : fond de l'écran du bas en attente
Les fonds sont stockés en JPEG (décodés par libjpeg sur la 3DS) et les chiffres
de l'IP / du port sont pré-dessinés dans la police du design (planche 512x32).
"""
import io
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ASSETS = os.path.join(ROOT, "3ds", "assets")
OUT = os.path.join(ROOT, "3ds", "source", "assets.h")
FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"

GLYPHS = "0123456789.:-"
ATLAS_W, ATLAS_H = 512, 32
DIGIT_H = 27          # même hauteur que « IP » et « Port » sur le design
BASELINE = 29         # ligne de base dans la planche


def jpeg_bytes(path, size):
    im = Image.open(path).convert("RGB")
    assert im.size == size, f"{path} doit faire {size[0]}x{size[1]} pixels"
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=95, subsampling=0)
    return buf.getvalue()


def make_atlas():
    # taille de police telle que les chiffres font DIGIT_H pixels de haut
    size = 10
    while True:
        f = ImageFont.truetype(FONT, size + 1)
        b = f.getbbox("0")
        if b[3] - b[1] > DIGIT_H:
            break
        size += 1
    font = ImageFont.truetype(FONT, size)
    ascent = font.getmetrics()[0]

    atlas = Image.new("L", (ATLAS_W, ATLAS_H), 0)
    draw = ImageDraw.Draw(atlas)
    table, x = [], 1
    for ch in GLYPHS:
        left, _, right, _ = font.getbbox(ch, stroke_width=1)
        w = right - left + 1
        draw.text((x - left, BASELINE - ascent), ch, font=font, fill=255,
                  stroke_width=1, stroke_fill=255)
        advance = round(font.getlength(ch)) + 1
        table.append((ch, x, w, advance))
        x += w + 2
    assert x <= ATLAS_W, "planche trop petite"
    return atlas, table


def c_array(name, data):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    top = jpeg_bytes(os.path.join(ASSETS, "top.png"), (400, 240))
    bottom = jpeg_bytes(os.path.join(ASSETS, "bottom.png"), (320, 240))
    atlas, table = make_atlas()
    atlas.save(os.path.join(ASSETS, "digits_preview.png"))

    parts = [
        "/* Fichier généré par tools/make_assets.py : ne pas modifier à la main. */",
        "#ifndef ASSETS_H", "#define ASSETS_H", "",
        c_array("ASSET_TOP_JPG", top), "",
        c_array("ASSET_BOTTOM_JPG", bottom), "",
        f"#define DIGITS_TEX_W {ATLAS_W}",
        f"#define DIGITS_TEX_H {ATLAS_H}",
        f"#define DIGITS_BASELINE {BASELINE}",
        "",
        c_array("ASSET_DIGITS_ALPHA", atlas.tobytes()), "",
        "typedef struct { char c; unsigned short x; unsigned char w; unsigned char advance; } DigitGlyph;",
        "static const DigitGlyph DIGIT_GLYPHS[] = {",
    ]
    for ch, x, w, adv in table:
        parts.append(f"    {{'{ch}', {x}, {w}, {adv}}},")
    parts += ["};", f"#define DIGIT_GLYPH_COUNT {len(table)}", "", "#endif", ""]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print(f"assets.h : fond haut {len(top)} o, fond bas {len(bottom)} o, {len(table)} chiffres")


if __name__ == "__main__":
    main()
