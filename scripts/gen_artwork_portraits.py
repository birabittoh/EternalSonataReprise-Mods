#!/usr/bin/env python3
"""Writes src/artwork_portraits_ps3 or src/artwork_portraits_360: every party
character's camp portraits redrawn from official artwork, as texture patches of
the game's camp art.

Needs a decoded data root of that console (EternalSonataReprise/assets/ps3-usa,
or assets-build/360-pal) for the shipped portraits' placement, and the cut out
artwork as PNGs. The 360 has ten party members, so Crescendo and Serenade are
left out there.

    python scripts/gen_artwork_portraits.py [--target ps3|360] [--data <root>] [--art <dir>] [--preview <dir>]

Each shipped portrait is a window onto the same render as the character's
status portrait, so the artwork is placed once, on the status portrait, by
lining up the eyes and scaling by the head, and the other portraits follow from
where they sit in that render (found by matching them against it).
"""

import argparse
import io
import os
import struct
import sys

import cv2
import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
GAME = os.path.normpath(os.path.join(ROOT, "..", "EternalSonataReprise"))
MODS = {"ps3": "artwork_portraits_ps3", "360": "artwork_portraits_360"}
DEFAULT_DATA = {"ps3": os.path.join(GAME, "assets", "ps3-usa"),
                "360": os.path.join(GAME, "assets-build", "360-pal")}
DEFAULT_ART = os.path.expanduser(os.path.join("~", "Pictures", "es"))

APPKEEP = "AppKeep.bmd"
CAMP = "campdata/camp_char.bmd"
KINDS = ("status", "panel", "face", "small_face")

# party number: (artwork, eyes in the status portrait, eyes in the artwork,
# artwork to status scale). The scale follows the head, which the artwork draws
# taller than the render in places, so it is a compromise of height and width.
PLACEMENT = {
    1: ("allegretto1", (213, 200), (250, 131), 0.90),
    2: ("polka1", (190, 206), (293, 223), 0.83),
    3: ("beat1", (190, 215), (284, 160), 0.80),
    4: ("frederic-francois-chopin2", (203, 202), (238, 182), 0.95),
    5: ("viola1", (178, 198), (432, 115), 0.95),
    6: ("salsa1", (190, 257), (278, 237), 0.70),
    7: ("jazz1", (215, 175), (388, 112), 0.81),
    8: ("falsetto1", (209, 205), (254, 154), 0.82),
    9: ("claves1", (233, 215), (228, 150), 0.85),
    10: ("march1", (228, 223), (245, 215), 0.78),
    11: ("prince-crescendo", (182, 175), (259, 128), 0.80),
    12: ("princess-serenade", (185, 195), (336, 158), 0.78),
}

# Small face and face, on top of the registered placement: party number to
# (shift right in px, zoom about the eyes, shift down in px).
SMALL_TWEAK = {3: (26, 1.0, 0), 7: (0, 1.22, 0), 8: (-12, 1.0, 0), 9: (32, 1.0, -10),
               10: (0, 1.0, 20)}

# The party member whose small face is the mod's icon.png, and where the square
# cut sits along the face's longer side (0 start, 1 end).
ICON = {"ps3": (12, 0.0), "360": (2, -0.1)}


def sources(c, target="ps3"):
    if target == "360":
        # Everything is an AppKeep entry; from Viola on the panel is the status art.
        status = (APPKEEP, 194 + c)
        panel = (APPKEEP, 204 + c) if c <= 4 else status
        return {"status": status, "panel": panel,
                "face": (APPKEEP, 208 + c), "small_face": (APPKEEP, 218 + c)}
    status = (APPKEEP, 193 + c) if c <= 10 else (APPKEEP, 204 + c - 11)
    panel = (CAMP, c - 1) if c <= 3 else (CAMP, 50) if c == 4 else status
    if c <= 3:
        face, small = (CAMP, 2 + c), (CAMP, 5 + c)
    elif c <= 10:
        face, small = (CAMP, 28 + c), (CAMP, 37 + c)
    else:
        face, small = (CAMP, 39 + c - 11), (CAMP, 48 + c - 11)
    return {"status": status, "panel": panel, "face": face, "small_face": small}


def be32(d, o):
    return struct.unpack(">I", d[o:o + 4])[0]


def le32(d, o):
    return struct.unpack("<I", d[o:o + 4])[0]


def read_bmd(path):
    data = open(path, "rb").read()
    if data[:4] not in (b"BMD ", b"CAMP"):
        # The 360's files are stored encoded; the host patches the decoded bytes.
        import models
        data = models.load_bmd(path)
    count = be32(data, 8)
    return data, [be32(data, 12 + 4 * i) for i in range(count)]


def texture_chunks(d):
    """Offsets of the texture chunks, in the order the host counts them, which
    is the ordinal a patch is named by."""
    found = []
    o = 0
    while o + 8 <= len(d):
        size = 0
        tag = d[o:o + 4]
        avail = len(d) - o
        if tag == b"NTEX" and avail >= 136:
            cs = be32(d, o + 4)
            if 136 <= cs <= avail and d[o + 8:o + 12] == b"DDS ":
                h, w = le32(d, o + 20), le32(d, o + 24)
                flags = le32(d, o + 88)
                ok = d[o + 92:o + 96] in (b"DXT1", b"DXT3", b"DXT5") if flags & 4 else le32(d, o + 96) == 32
                if w and h and w <= 8192 and h <= 8192 and ok:
                    size = cs
        elif tag == b"NTX2" and avail >= 0x80 and d[o + 8:o + 12] == b"XPR2" and d[o + 0x18:o + 0x1C] == b"TX2D":
            cs, hs, ps, ns = be32(d, o + 4), be32(d, o + 12), be32(d, o + 16), be32(d, o + 0x1C)
            pa = (o + 8 + hs + 0xFFF) & ~0xFFF
            fetch = 0x18 + ns + 0x18
            if 0x80 <= cs <= avail and 0 < ps <= cs and pa >= o and pa - o + ps <= cs and fetch + 24 <= avail:
                w1, w2 = be32(d, o + fetch + 4), be32(d, o + fetch + 8)
                if (w1 & 0x3F) in (6, 18, 19, 20) and (w2 & 0x1FFF) < 8192 and ((w2 >> 13) & 0x1FFF) < 8192:
                    size = cs
        if size:
            found.append(o)
            o += size
        else:
            o += 1
    return found


def load_ntex(data, start):
    size = be32(data, start + 4)
    if data[start:start + 4] == b"NTX2":
        import models
        return Image.open(io.BytesIO(models.texture_dds(data, models.Texture(data, start, size)))).convert("RGBA")
    return Image.open(io.BytesIO(data[start + 8:start + size])).convert("RGBA")


def register(src, dst):
    """f, ox, oy such that dst ~ f * src + (ox, oy), by template matching."""
    s = np.array(src)[:, :, [2, 1, 0, 3]]
    d = np.array(dst)[:, :, [2, 1, 0, 3]]
    m = cv2.erode((s[:, :, 3] > 250).astype(np.uint8), np.ones((9, 9), np.uint8))
    ys, xs = np.where(m > 0)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    t = s[y0:y1, x0:x1, :3].astype(np.float32)
    tm = m[y0:y1, x0:x1]
    a = d[:, :, 3:4] / 255.0
    dd = (d[:, :, :3] * a + np.array([160, 140, 120]) * (1 - a)).astype(np.float32)
    best = (-1.0, 1.0, 0, 0)
    for f in np.arange(0.95, 1.35, 0.01):
        tw, th = int(t.shape[1] * f), int(t.shape[0] * f)
        if tw < 24 or th < 24 or tw > dd.shape[1] or th > dd.shape[0]:
            continue
        tt = cv2.resize(t, (tw, th), interpolation=cv2.INTER_AREA)
        mm = cv2.resize(tm, (tw, th), interpolation=cv2.INTER_NEAREST)
        mm3 = np.repeat(mm[:, :, None], 3, 2).astype(np.float32)
        r = np.nan_to_num(cv2.matchTemplate(dd, tt, cv2.TM_CCORR_NORMED, mask=mm3))
        _, mx, _, loc = cv2.minMaxLoc(r)
        if mx > best[0]:
            best = (mx, f, loc[0], loc[1])
    score, f, lx, ly = best
    if score < 0.95:
        sys.exit(f"error: portrait match scored only {score:.3f}")
    return f, lx - f * x0, ly - f * y0


def defringe(art, grow=2):
    """The cut outs keep a pale rim of their white background. Pull the edge in
    and repaint the rim with the colour just inside it."""
    a = np.array(art).astype(np.float32)
    inner = cv2.erode((a[:, :, 3] > 200).astype(np.uint8),
                      cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * grow + 1,) * 2))
    weight = inner.astype(np.float32)
    rgb = a[:, :, :3] * weight[:, :, None]
    for radius in (3, 7, 15):
        k = (radius * 2 + 1,) * 2
        den = cv2.blur(weight, k)
        fill = cv2.blur(rgb, k) / np.maximum(den[:, :, None], 1e-4)
        use = (weight < 0.5) & (den > 1e-3)
        rgb = np.where(use[:, :, None], fill, rgb)
        weight = np.maximum(weight, (den > 1e-3).astype(np.float32))
    edge = cv2.GaussianBlur(inner.astype(np.float32), (0, 0), 0.8)
    return Image.fromarray(np.dstack([rgb, edge * 255]).clip(0, 255).astype(np.uint8), "RGBA")


def place(art, size, scale, anchor_art, anchor_out):
    """The artwork scaled by `scale` with its anchor on anchor_out, in a canvas of `size`."""
    nw, nh = max(1, round(art.width * scale)), max(1, round(art.height * scale))
    premult = np.array(art).astype(np.float32)
    premult[:, :, :3] *= premult[:, :, 3:4] / 255.0
    big = cv2.resize(premult, (nw, nh), interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_CUBIC)
    tx = anchor_out[0] - anchor_art[0] * scale
    ty = anchor_out[1] - anchor_art[1] * scale
    out = cv2.warpAffine(big, np.float32([[1, 0, tx], [0, 1, ty]]), size, flags=cv2.INTER_LINEAR,
                         borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0, 0))
    alpha = np.clip(out[:, :, 3:4], 0, 255)
    rgb = out[:, :, :3] / np.maximum(alpha / 255.0, 1e-4)
    return Image.fromarray(np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8), "RGBA")


def icon(img, along):
    """The largest square of the opaque part, scaled to 256."""
    x0, y0, x1, y1 = img.getchannel("A").point(lambda v: 255 if v > 16 else 0).getbbox()
    side = min(x1 - x0, y1 - y0)
    x = x0 + round((x1 - x0 - side) * along)
    y = y0 + round((y1 - y0 - side) * along)
    return img.crop((x, y, x + side, y + side)).resize((256, 256), Image.LANCZOS)


def windows(images_by_char):
    """Per (kind, size), the union of the vanilla opaque boxes: the area the game's frame expects."""
    win = {}
    for images in images_by_char:
        for kind, (_, _, van) in images.items():
            ys, xs = np.where(np.array(van)[:, :, 3] > 16)
            box = (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)
            old = win.get((kind, van.size))
            win[(kind, van.size)] = box if old is None else (
                min(old[0], box[0]), min(old[1], box[1]), max(old[2], box[2]), max(old[3], box[3]))
    return win


def clip(img, box):
    a = np.array(img)
    m = np.zeros(a.shape[:2], bool)
    m[box[1]:box[3], box[0]:box[2]] = True
    a[~m, 3] = 0
    return Image.fromarray(a)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=sorted(MODS), default="ps3")
    ap.add_argument("--data", help="decoded data root; default is the repo's for the target")
    ap.add_argument("--art", default=DEFAULT_ART)
    ap.add_argument("--preview", help="also write before and after sheets here")
    args = ap.parse_args()
    global MOD
    MOD = os.path.join(ROOT, "src", MODS[args.target])
    args.data = args.data or (os.environ.get("ES_PS3_DATA_ROOT") if args.target == "ps3" else None)         or DEFAULT_DATA[args.target]
    party = {c: row for c, row in PLACEMENT.items() if args.target == "ps3" or c <= 10}
    sys.path.insert(0, os.path.join(GAME, "scripts"))

    files = {}
    for name in (APPKEEP, CAMP) if args.target == "ps3" else (APPKEEP,):
        path = os.path.join(args.data, name)
        if not os.path.isfile(path):
            sys.exit(f"error: {path} not found; pass --data")
        data, starts = read_bmd(path)
        files[name] = (data, starts, texture_chunks(data))
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)

    loaded = {}
    for c in party:
        loaded[c] = {}
        for kind, (f, e) in sources(c, args.target).items():
            data, starts, chunks = files[f]
            loaded[c][kind] = (f, chunks.index(starts[e]), load_ntex(data, starts[e]))
    win = windows(loaded.values())

    count = 0
    for c, (art_name, v_eye, a_eye, s) in party.items():
        art = defringe(Image.open(os.path.join(args.art, f"es-{art_name}.png")).convert("RGBA"))
        images = loaded[c]
        status = images["status"][2]
        done = set()
        before, after = [], []
        for kind in KINDS:
            f, ordinal, van = images[kind]
            if kind == "status":
                scale, eye = s, v_eye
            else:
                fac, ox, oy = register(van, status)
                scale, eye = s / fac, ((v_eye[0] - ox) / fac, (v_eye[1] - oy) / fac)
                if kind in ("small_face", "face") and c in SMALL_TWEAK:
                    dx, zoom, dy = SMALL_TWEAK[c]
                    scale, eye = scale * zoom, (eye[0] + dx, eye[1] + dy)
            new = clip(place(art, van.size, scale, a_eye, eye), win[(kind, van.size)])
            before.append(van)
            after.append(new)
            if kind == "small_face" and c == ICON[args.target][0]:
                icon(new, ICON[args.target][1]).save(os.path.join(MOD, "icon.png"), optimize=True)
            if (f, ordinal) in done:
                continue
            done.add((f, ordinal))
            out = os.path.join(MOD, "assets", f.lower(), "textures", f"{ordinal}.png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            new.save(out, optimize=True)
            count += 1
        if args.preview:
            hgt = 360
            tiles = [t.resize((max(1, t.width * hgt // t.height), hgt), Image.LANCZOS)
                     for pair in zip(before, after) for t in pair]
            sheet = Image.new("RGBA", (sum(t.width for t in tiles) + 8 * len(tiles), hgt), (120, 140, 160, 255))
            x = 0
            for t in tiles:
                sheet.alpha_composite(t, (x, 0))
                x += t.width + 8
            sheet.convert("RGB").save(os.path.join(args.preview, f"c{c:02d}.png"))
    print(f"artwork_portraits: wrote {count} textures to {MOD}")


if __name__ == "__main__":
    main()
