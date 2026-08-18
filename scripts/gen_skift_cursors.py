#!/usr/bin/env python3
"""Render Skift's exact vector cursors to PNG (kora/cursors/skift-*.png).

Skift's strata-shell draws its cursor as a vector path: black fill + white
1.6px stroke, 28x28 (arrow) / 32x32 (resize).  The paths below are copied
verbatim from skift/src/srvs/strata-shell/defs/{rounded,classic}-cursor.path
and input.cpp (ResizeCursor).  We rasterise them here at native size so the
compositor's raster cursor looks identical to Skift's.

Outputs (for each kind) a .png plus a .hot sidecar ("hotx,hoty") that
gen_cursors.py uses to place the hotspot precisely.
"""
import os, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "kora", "cursors")

# Skift cursor paths (verbatim).
ROUNDED = ("m2 11 .0035 10.4019c.0006 2.9878 3.7634 4.3068 5.6294 1.9734"
           "l3.1863-3.9846c.3854-.482.9627-.7708 1.5795-.7902l5.0994-.1604"
           "c2.9864-.0939 4.1873-3.896 1.7966-5.6882L5.3712 2.3136"
           "C3.9818 1.272 1.9997 2.2636 2 4z")
CLASSIC = ("m5.84 11.12 2.64 6.08-2.16.96-2.64-6.08-2.88 3.04v-13.52l9.36 9.44z")
RESIZE  = "M-11 0L-5 -5L-5 -2L5 -2L5 -5L11 0L5 5L5 2L-5 2L-5 5Z"

def render(d, size, origin, name):
    """Render path `d` into a `size`x`size` transparent PNG, the path
    translated so its (0,0) lands at `origin` (x,y)."""
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {size} {size}">
<g transform="translate({origin[0]},{origin[1]})">
<path d="{d}" fill="#000000" stroke="#ffffff" stroke-width="1.6" stroke-linejoin="miter"/>
</g>
</svg>'''
    with tempfile.TemporaryDirectory() as td:
        s = os.path.join(td, "c.svg")
        p = os.path.join(td, "c.png")
        open(s, "w").write(svg)
        subprocess.run(["rsvg-convert", "-o", p, s], check=True)
        from PIL import Image
        im = Image.open(p).convert("RGBA")
        dst = os.path.join(OUT, name + ".png")
        im.save(dst)
        print(f"  + {dst} ({size}x{size})")
    # hotspot sidecar
    with open(os.path.join(OUT, name + ".hot"), "w") as f:
        f.write(f"{origin[0]},{origin[1]}\n")

def main():
    os.makedirs(OUT, exist_ok=True)
    # arrow: rounded cursor, tip (path origin) at canvas (1,1) so the 1.6px
    # white stroke can bleed 0.8px outside without clipping.
    render(ROUNDED, 30, (1, 1), "skift-arrow")
    # hand: Skift has no hand cursor (the arrow stays an arrow); we use the
    # classic sharp arrow for interactive elements as a subtle distinction.
    render(CLASSIC, 30, (1, 1), "skift-hand")
    # resize: 4-way arrow centred on the hotspot.
    render(RESIZE, 34, (17, 17), "skift-resize")

if __name__ == "__main__":
    main()
