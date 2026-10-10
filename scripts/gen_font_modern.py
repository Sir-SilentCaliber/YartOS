#!/usr/bin/env python3
"""Render an antialiased bitmap font header (userland/font_modern.h).

Uses Inter Medium (Skift's label font, 12px) for the whole UI.
old DejaVu Sans Bold, so the desktop text stops looking thick/cartoony.

Output format matches the existing font_modern.h:
  #define MODERN_FONT_W 10
  #define MODERN_FONT_H 18
  static const unsigned char modern_font_aa[95][18][10] = { ... };
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "userland", "font_modern.h")

W, H = 12, 18
BASELINE = 14          # baseline row inside the 18px cell
SS = 4                 # supersample factor for smooth AA

CANDIDATES = [
    os.path.join(ROOT, "kora", "fonts", "Inter-Medium.ttf"),
    os.path.join(ROOT, "kora", "fonts", "Inter-Regular.ttf"),
    "/tmp/inter/extras/ttf/Inter-Medium.ttf",
    "/tmp/inter/extras/ttf/Inter-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]

def load_font():
    for p in CANDIDATES:
        if not os.path.exists(p):
            continue
        for sz in (13, 12, 11):
            fnt = ImageFont.truetype(p, sz * SS)
            asc, desc = fnt.getmetrics()
            # scale metrics back to cell space
            asc /= SS; desc /= SS
            if asc + desc > H - 1:
                continue
            # widest glyph must fit in W-1 (leave 1px side bearing)
            max_w = 0
            for cp in range(0x21, 0x7F):
                bb = fnt.getbbox(chr(cp))
                if bb:
                    w = (bb[2] - bb[0]) / SS
                    max_w = max(max_w, w)
            if max_w <= W:
                return fnt, p, sz, asc, desc
    raise SystemExit("no usable TTF found")

def render_glyph(fnt, ch):
    bw, bh = W * SS, H * SS
    img = Image.new("L", (bw, bh), 0)
    d = ImageDraw.Draw(img)
    asc, _ = fnt.getmetrics()
    # draw.text y = top of ascent; we want baseline at BASELINE*SS
    y = int(BASELINE * SS - asc)
    d.text((0, y), ch, font=fnt, fill=255)
    img = img.resize((W, H), Image.LANCZOS)
    px = img.load()
    rows = []
    for yy in range(H):
        row = []
        for xx in range(W):
            row.append(px[xx, yy])
        rows.append(row)
    return rows

def main():
    fnt, path, sz, asc, desc = load_font()
    print(f"using {path} @ {sz}px (ascent {asc:.1f} desc {desc:.1f})")

    # proportional advance widths, measured at logical size (Skift's UI font
    # is proportional; our old monospace 12px cells made letters look too far
    # apart).
    fnt_m = ImageFont.truetype(path, sz)
    adv = []
    for cp in range(0x20, 0x7F):
        w = fnt_m.getlength(chr(cp))
        a = int(round(w))
        if a < 1: a = 1
        if a > W: a = W
        adv.append(a)

    lines = []
    lines.append("#pragma once")
    lines.append("/* Antialiased proportional font - Inter Medium 12px (Skift label font). */")
    lines.append(f"#define MODERN_FONT_W {W}")
    lines.append(f"#define MODERN_FONT_H {H}")
    lines.append("/* Per-glyph advance width (proportional, not monospace). */")
    lines.append("static const unsigned char modern_font_adv[95] = {")
    for i in range(0, 95, 12):
        lines.append("    " + ", ".join(str(a) for a in adv[i:i+12]) + ",")
    lines.append("};")
    lines.append(f"static const unsigned char modern_font_aa[95][{H}][{W}] = {{")
    for cp in range(0x20, 0x7F):
        ch = chr(cp)
        label = ch if ch not in ('\\', "'", '"') else ' '
        rows = render_glyph(fnt, ch)
        lines.append(f"  /* 0x{cp:02X} '{label}' */ {{")
        for r in rows:
            lines.append("    { " + ", ".join(str(v) for v in r) + " },")
        lines.append("  },")
    lines.append("};")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT} ({len(lines)} lines)")

if __name__ == "__main__":
    main()
