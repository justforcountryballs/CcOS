#!/usr/bin/env python3
# Builds ccos's emoji sheet: every fully-qualified emoji (no skin-tone variants),
# drawn with Noto Color Emoji at 32x32, with its name and group, gzip'd.
import sys, gzip, struct
from PIL import Image, ImageDraw, ImageFont
test, font_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
S = 32
GROUPS = ["Smileys & Emotion", "People & Body", "Animals & Nature", "Food & Drink", "Travel & Places", "Activities", "Objects", "Symbols", "Flags"]
font = ImageFont.truetype(font_path, 109)
items = []; g = None
for line in open(test, encoding="utf-8"):
    if line.startswith("# group:"):
        name = line.split(":", 1)[1].strip(); g = GROUPS.index(name) if name in GROUPS else None; continue
    if line.startswith("#") or ";" not in line or g is None: continue
    cps, rest = line.split(";", 1)
    if "fully-qualified" not in rest: continue
    cps = [int(c, 16) for c in cps.split()]
    if any(0x1F3FB <= c <= 0x1F3FF for c in cps): continue          # skin tones
    nm = rest.split("#", 1)[1].strip().split(" ", 2)[2]               # "😀 E1.0 grinning face"
    ch = "".join(chr(c) for c in cps)
    im = Image.new("RGBA", (136, 128), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((0, 0), ch, font=font, embedded_color=True)
    bb = im.getbbox()
    if not bb: continue
    im = im.crop(bb)
    w, h = im.size
    if w > 140 or w > h * 1.6: continue                               # the font drew it as several pictures
    side = max(w, h); sq = Image.new("RGBA", (side, side), (0, 0, 0, 0)); sq.paste(im, ((side - w) // 2, (side - h) // 2))
    sq = sq.resize((S, S), Image.LANCZOS)
    items.append((g, ch.encode(), nm.encode()[:120], sq.tobytes()))
with gzip.open(out, "wb", 9) as f:
    f.write(b"CCEMOJI1" + struct.pack("<II", len(items), S))
    for g, u, n, px in items: f.write(struct.pack("<BBB", g, len(u), len(n)) + u + n)
    for g, u, n, px in items: f.write(px)
print(len(items), "emoji")
