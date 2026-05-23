#!/usr/bin/env python3
"""
Yart OS - render a real 8x16 mono bitmap font from a TTF.

Output: kernel/gui/font_data.c -- declares const u8 yart_font8x16[256][16].

We pick a font size that fits ALL printable ASCII inside an 8-wide cell,
and center each glyph vertically so caps + descenders both fit in the 16
row cell.  Threshold is chosen high to avoid antialias fringe pixels
becoming spurious dots.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
THRESHOLD = 128   # gray > 128 -> on; lower values become background

CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
]

def load_font():
    for p in CANDIDATES:
        if not os.path.exists(p): continue
        for sz in (16, 15, 14, 13, 12, 11, 10):
            fnt = ImageFont.truetype(p, sz)
            # measure widest printable
            max_w = 0
            for cp in range(0x21, 0x7F):
                bb = fnt.getbbox(chr(cp))
                if bb: max_w = max(max_w, bb[2] - bb[0])
            # measure tallest extent (cap height + descender)
            asc, desc = fnt.getmetrics()
            total_h = asc + desc
            if max_w <= W and total_h <= H + 2:
                return fnt, p, sz, asc, desc
    raise SystemExit("No usable mono TTF found.")

def render_glyph(fnt, ch, asc, desc):
    # render at 3x for better AA -> downsample? simpler: render direct.
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    bbox = fnt.getbbox(ch)
    if bbox is None or bbox[2] - bbox[0] == 0:
        return [0]*H
    ch_w = bbox[2] - bbox[0]
    x0 = (W - ch_w) // 2 - bbox[0]
    # baseline placement: align so caps top at y=2, baseline at y = 2+asc
    # i.e. subtract bbox[1] to align top-left of glyph to (x0, 2)
    # Actually: draw uses y as the top of font box. We want top-of-cap at row 2.
    y0 = 2 - bbox[1] + (asc - (bbox[3] - bbox[1])) // 4   # nudge so 'g' descender fits
    d.text((x0, y0), ch, font=fnt, fill=255)
    rows = []
    px = img.load()
    for y in range(H):
        b = 0
        for x in range(W):
            if px[x, y] > THRESHOLD:
                b |= (1 << (7 - x))
        rows.append(b)
    return rows

def fallback_dot():
    rows = [0]*H
    for y in range(7, 11):
        rows[y] = 0b00111100
    return rows

def main(out_path):
    fnt, src, sz, asc, desc = load_font()
    print(f"using {src} @ {sz}pt (asc={asc} desc={desc})", file=sys.stderr)

    glyphs = {0x20: [0]*H}
    for cp in range(0x21, 0x7F):
        glyphs[cp] = render_glyph(fnt, chr(cp), asc, desc)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write(f"/* AUTO-GENERATED from {os.path.basename(src)} @ {sz}pt */\n")
        f.write('#include <yart/types.h>\n#include <yart/gui.h>\n\n')
        f.write("const u8 yart_font8x16[256][16] = {\n")
        for cp in range(256):
            rows = glyphs.get(cp, fallback_dot()) if 0x20 <= cp < 0x7F else fallback_dot()
            label = repr(chr(cp)) if 0x20 <= cp < 0x7F else "?"
            f.write(f"  /* 0x{cp:02X} {label} */ {{ ")
            f.write(",".join(f"0x{b:02X}" for b in rows))
            f.write(" },\n")
        f.write("};\n")
    print("wrote", out_path)

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "kernel/gui/font_data.c"
    main(target)
