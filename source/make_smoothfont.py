# Pre-render DejaVu Sans Mono into ccos's character grid (CP437 order), anti-aliased.
#   small: 8x16 cells (regular, bold, italic)   large: 16x32 cells (regular, bold)
import sys
from PIL import Image, ImageDraw, ImageFont

D = "/usr/share/fonts/truetype/dejavu/"
# CP437 code page 0x00-0x1F and 0x7F are pictures in the original IBM font
LOW = " ☺☻♥♦♣♠•◘○◙♂♀♪♫☼" \
      "►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
def cp437_char(i):
    if i < 32: return LOW[i]
    if i == 0x7f: return "⌂"
    return bytes([i]).decode("cp437")

# after the 256 CP437 characters: the Polish letters (glyphs 256..273)
EXTRA = "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ"
NGLYPH = 256 + len(EXTRA)

def render(path, size, cw, ch, baseline):
    f = ImageFont.truetype(path, size)
    out = bytearray(NGLYPH * cw * ch)
    missing = []
    for i in range(NGLYPH):
        c = cp437_char(i) if i < 256 else EXTRA[i - 256]
        if i == 0 or c == " ": continue
        img = Image.new("L", (cw * 3, ch * 2), 0)
        d = ImageDraw.Draw(img)
        adv = f.getlength(c)
        x = cw + (cw - adv) / 2.0                    # centre the glyph in its cell
        d.text((x, ch // 2 + baseline), c, font=f, fill=255, anchor="ls")
        cell = img.crop((cw, ch // 2, cw * 2, ch // 2 + ch))
        # box-drawing and block characters must reach the cell edges to join up
        if 0xb0 <= i <= 0xdf and i < 256:
            full = img.crop((cw - 1, ch // 2, cw * 2 + 1, ch // 2 + ch)).resize((cw, ch), Image.LANCZOS)
            cell = full
        if ch == 16 and "Bold" not in path:          # small text: a touch heavier so it reads well
            cell = cell.point(lambda a: int(255 * (a / 255.0) ** 0.72 + 0.5))
        px = cell.tobytes()
        if not any(px): missing.append(i)
        out[i * cw * ch:(i + 1) * cw * ch] = px
    return out, missing

def emit(name, data, cw, ch, fh):
    fh.write("static const unsigned char %s[%d * %d] = {\n" % (name, NGLYPH, cw * ch))
    for k in range(0, len(data), 32):
        fh.write("  " + ",".join(str(b) for b in data[k:k + 32]) + ",\n")
    fh.write("};\n")

if __name__ == "__main__":
    out = sys.argv[1]
    sets = [
        ("sf_reg",   D + "DejaVuSansMono.ttf",         13.3, 8, 16, 12),
        ("sf_bold",  D + "DejaVuSansMono-Bold.ttf",    13.3, 8, 16, 12),
        ("sf_ital",  D + "DejaVuSansMono-Oblique.ttf", 13.3, 8, 16, 12),
        ("sf2_reg",  D + "DejaVuSansMono.ttf",         26.6, 16, 32, 25),
        ("sf2_bold", D + "DejaVuSansMono-Bold.ttf",    26.6, 16, 32, 25),
    ]
    with open(out, "w") as fh:
        fh.write("/* Smooth font for ccos: DejaVu Sans Mono (Bitstream Vera / DejaVu licence),\n"
                 " * pre-rendered by tools/make_smoothfont.py. One alpha byte per pixel, CP437 order,\n"
                 " * then the Polish letters (glyphs 256..273: %s). */\n" % EXTRA)
        for name, path, size, cw, ch, base in sets:
            data, missing = render(path, size, cw, ch, base)
            emit(name, data, cw, ch, fh)
            print(name, "missing glyphs:", [hex(m) for m in missing if m not in (0, 0x20, 0xff)])
