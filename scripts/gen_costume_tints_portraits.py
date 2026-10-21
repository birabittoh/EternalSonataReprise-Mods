#!/usr/bin/env python3
"""Writes the portraits of the characters whose art is too pale to tint: their
own camp art with a dot in the costume's accent colour.

They are derived from game data, so they are generated here once and committed
under src/costume_tints/costumes/ instead of in CI. Reads a decoded PS3 data
tree (EternalSonataReprise/assets/ps3-usa has what is needed).

    python scripts/gen_costume_tints_portraits.py [--data <PS3 data root>]
"""

import argparse
import io
import os
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
MOD = os.path.join(ROOT, "src", "costume_tints")
DEFAULT_DATA = os.path.normpath(os.path.join(ROOT, "..", "EternalSonataReprise", "assets", "ps3-usa"))
CHARACTERS = {"serenade": 12}
KINDS = ("status", "face", "small_face")  # the panel shows the status art
HERE = MOD

APPKEEP = "AppKeep.bmd"
CAMP = "campdata/camp_char.bmd"

def sources(c):
    status = (APPKEEP, 193 + c) if c <= 10 else (APPKEEP, 204 + c - 11)
    if c <= 3:
        panel = (CAMP, c - 1)
    elif c == 4:
        panel = (CAMP, 50)
    else:
        panel = status  # the panel of 5..12 shows the status art
    if c <= 3:
        face, small = (CAMP, 2 + c), (CAMP, 5 + c)
    elif c <= 10:
        face, small = (CAMP, 28 + c), (CAMP, 37 + c)
    else:
        face, small = (CAMP, 39 + c - 11), (CAMP, 48 + c - 11)
    return {"status": status, "panel": panel, "face": face, "small_face": small}




def read_bmd(path):
    data = open(path, "rb").read()
    if data[:4] not in (b"BMD ", b"CAMP"):
        sys.exit(f"error: {path} is not a decoded .bmd")
    count = struct.unpack(">I", data[8:12])[0]
    starts = [struct.unpack(">I", data[12 + 4 * i:16 + 4 * i])[0] for i in range(count)]
    entries = []
    for start in starts:
        end = min([o for o in starts if o > start] + [len(data)])
        entries.append(data[start:end] if start else b"")
    return entries


def dds_image(entry):
    if entry[:4] != b"NTEX":
        sys.exit("error: entry is not an NTEX texture")
    return Image.open(io.BytesIO(entry[8:])).convert("RGBA")




def dxt5(image):
    out = io.BytesIO()
    image.save(out, "DDS", pixel_format="DXT5")
    return out.getvalue()


def write_dds(image, path):
    """DXT5 with a full mipmap chain, as the originals have."""
    levels = []
    level = image
    while True:
        levels.append(dxt5(level)[128:])
        if level.width == 1 and level.height == 1:
            break
        level = level.resize((max(1, level.width // 2), max(1, level.height // 2)),
                             Image.LANCZOS)
    header = bytearray(dxt5(image)[:128])
    struct.pack_into("<I", header, 8, struct.unpack_from("<I", header, 8)[0] | 0x20000)
    struct.pack_into("<I", header, 28, len(levels))
    struct.pack_into("<I", header, 108, struct.unpack_from("<I", header, 108)[0] | 0x400008)
    with open(path, "wb") as f:
        f.write(header)
        for data in levels:
            f.write(data)




def accents(character):
    """(costume name, accent colour) in assets.toml order."""
    text = open(os.path.join(HERE, "assets.toml"), encoding="utf8").read()
    found = []
    for block in text.split("[[costume]]")[1:]:
        if f'character = "{character}"' not in block:
            continue
        name = re.search(r'name = "(\w+)"', block).group(1)
        color = re.search(r'to = "#(\w{6})"', block).group(1)
        found.append((name, tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))))
    return found


def shade(color, factor):
    return tuple(int(c * factor) for c in color)


def with_dot(image, color):
    """A glossy dot ringed in a darker shade of itself, over a soft shadow, so
    it reads on the white art whatever its colour."""
    scale = 4
    w, h = image.size
    r = max(5, min(w, h) * 9 // 100)
    margin = max(3, r // 2)
    cx, cy = r + margin, h - r - margin
    size = (w * scale, h * scale)

    def disc(radius, dx=0, dy=0):
        mask = Image.new("L", size, 0)
        ImageDraw.Draw(mask).ellipse(
            ((cx + dx - radius) * scale, (cy + dy - radius) * scale,
             (cx + dx + radius) * scale, (cy + dy + radius) * scale), fill=255)
        return mask

    def paint(layer, mask, rgb, alpha=255):
        solid = Image.new("RGBA", size, rgb + (alpha,))
        solid.putalpha(mask.point(lambda v: v * alpha // 255))
        return Image.alpha_composite(layer, solid)

    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow = disc(r, 0, max(1, r // 8)).filter(ImageFilter.GaussianBlur(r * scale // 7))
    layer = paint(layer, shadow, (0, 0, 0), 110)
    layer = paint(layer, disc(r), shade(color, 0.62))
    layer = paint(layer, disc(r * 82 // 100), color)
    gloss = disc(r * 40 // 100, -r * 22 // 100, -r * 28 // 100).filter(
        ImageFilter.GaussianBlur(r * scale // 14))
    layer = paint(layer, gloss, (255, 255, 255), 120)
    return Image.alpha_composite(image, layer.resize((w, h), Image.LANCZOS))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", default=os.environ.get("ES_PS3_DATA_ROOT", DEFAULT_DATA),
                        help="PS3 data root (default: %(default)s)")
    args = parser.parse_args()
    if not os.path.isdir(args.data):
        sys.exit(f"error: {args.data} not found; pass --data or set ES_PS3_DATA_ROOT")

    files = {name: read_bmd(os.path.join(args.data, name)) for name in (APPKEEP, CAMP)}
    out_dir = os.path.join(MOD, "costumes")
    os.makedirs(out_dir, exist_ok=True)
    count = 0
    for character, c in CHARACTERS.items():
        srcs = sources(c)
        for kind in KINDS:
            file, entry = srcs[kind]
            base = dds_image(files[file][entry])
            for name, color in accents(character):
                write_dds(with_dot(base, color),
                          os.path.join(out_dir, f"{character}_{name}_{kind}.dds"))
                count += 1
    print(f"costume_tints: wrote {count} portraits to {out_dir}")


if __name__ == "__main__":
    main()
