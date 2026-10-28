#!/usr/bin/env python3
"""Previews the portrait tints of a costume mod's assets.toml on the game's
art: one sheet per character, the untinted portraits on top and a row per
costume below, every portrait kind across. Reads a decoded PS3 data tree.

    python scripts/preview_costume_tints.py OUT_DIR [--mod costume_tints]
        [--character polka] [--mask] [--grid] [--data <PS3 data root>]

--mask paints what each costume's rules catch red instead of tinting, and
--grid draws a 32 pixel grid in the portraits' own pixels, to read off rects.
Costumes made from another than the default are left out. The tint maths is
EternalSonataReprise's scripts/tint_preview.py.
"""

import argparse
import io
import os
import struct
import sys
import tomllib

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
GAME = os.path.normpath(os.path.join(ROOT, "..", "EternalSonataReprise"))
sys.path.insert(0, os.path.join(GAME, "scripts"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tint_preview as T  # noqa: E402

CHARACTERS = ["allegretto", "polka", "beat", "frederic", "viola", "salsa", "jazz",
              "falsetto", "claves", "march", "crescendo", "serenade"]
KINDS = ("status", "panel", "face", "small_face")
APPKEEP = "appkeep.bmd"
CAMP = "campdata/camp_char.bmd"


def sources(c):
    """Party member c's portraits as (file, entry) by kind, on PS3 data."""
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
    """A decoded .bmd's entries, b"" for a null one."""
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


def gridded(image):
    out = image.copy()
    d = ImageDraw.Draw(out)
    for x in range(0, image.width, 32):
        d.line([(x, 0), (x, image.height)], fill=(255, 255, 0, 255 if x % 128 == 0 else 110))
        if x % 64 == 0:
            d.text((x + 2, 2), str(x), fill=(255, 255, 255, 255))
    for y in range(0, image.height, 32):
        d.line([(0, y), (image.width, y)], fill=(255, 255, 0, 255 if y % 128 == 0 else 110))
        if y % 64 == 0:
            d.text((2, y + 2), str(y), fill=(255, 255, 255, 255))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out")
    parser.add_argument("--mod", default="costume_tints")
    parser.add_argument("--character", action="append")
    parser.add_argument("--mask", action="store_true")
    parser.add_argument("--grid", action="store_true")
    parser.add_argument("--size", type=int, default=256)
    parser.add_argument("--data", default=os.environ.get(
        "ES_PS3_DATA_ROOT", os.path.join(GAME, "assets", "ps3-usa")))
    args = parser.parse_args()

    mod_root = os.path.join(ROOT, "src", args.mod)
    with open(os.path.join(mod_root, "assets.toml"), "rb") as f:
        declared = tomllib.load(f)
    costumes = declared.get("costume", [])
    regions = {(r["character"], r["name"]): r for r in declared.get("portrait_region", [])}
    bmds = {name: read_bmd(os.path.join(args.data, name)) for name in (APPKEEP, CAMP)}
    os.makedirs(args.out, exist_ok=True)
    for c, character in enumerate(CHARACTERS, 1):
        if args.character and character not in args.character:
            continue
        mine = [x for x in costumes if x.get("character") == character and
                x.get("base", "default") == "default" and x.get("portrait_tint")]
        if not mine:
            continue
        portraits = {kind: bmds[f][e][8:] for kind, (f, e) in sources(c).items()}
        size = args.size
        sheet = Image.new("RGBA", (size * len(KINDS), size * (len(mine) + 1)), (60, 60, 60, 255))
        draw = ImageDraw.Draw(sheet)
        for row, costume in enumerate([None] + mine):
            for col, kind in enumerate(KINDS):
                dds = portraits[kind]
                for rule in (costume or {}).get("portrait_tint", []):
                    if kind not in rule.get("kinds", KINDS):
                        continue
                    layers = [rule]
                    if "region" in rule:
                        layers.insert(0, regions[(character, rule["region"])])
                    region = T.kind_region(layers, kind)
                    dds = T.tint_dds(dds, T.rule_fn(rule, args.mask), region, mod_root)
                image = Image.open(io.BytesIO(dds)).convert("RGBA")
                if args.grid:
                    image = gridded(image)
                image.thumbnail((size, size))
                sheet.alpha_composite(image, (col * size, row * size))
            draw.text((4, row * size + size - 14), costume["name"] if costume else "game",
                      fill=(255, 255, 255, 255))
        sheet.save(os.path.join(args.out, f"{c:02}_{character}.png"))
        print(f"{character}: {len(mine)} costumes")


if __name__ == "__main__":
    main()
