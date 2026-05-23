#!/usr/bin/env python3
"""Render the wallpaper into a real 24-bit BMP that gets bundled into
   /etc/wallpaper.bmp on the initrd. The kernel will load it through
   the BMP decoder."""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

W, H = 1280, 800
BG_TOP = (27, 31, 36)
BG_BOT = (17, 20, 26)
DOT    = (44, 50, 62)
ACCENT = (232, 168, 124)
COOL   = (80, 110, 160)

img = Image.new("RGB", (W, H), BG_BOT)
px = img.load()
for y in range(H):
    t = y / (H - 1)
    r = int(BG_TOP[0] + (BG_BOT[0]-BG_TOP[0]) * t)
    g = int(BG_TOP[1] + (BG_BOT[1]-BG_TOP[1]) * t)
    b = int(BG_TOP[2] + (BG_BOT[2]-BG_TOP[2]) * t)
    for x in range(W):
        px[x, y] = (r, g, b)

# faint dot grid
d = ImageDraw.Draw(img)
for y in range(48, H, 48):
    for x in range(48, W, 48):
        d.point((x, y), fill=DOT)

# warm corner glow (top-left) and cool counter-glow (bottom-right)
glow = Image.new("RGBA", (W, H), (0,0,0,0))
gd = ImageDraw.Draw(glow)
gd.ellipse((-300, -300, 700, 700), fill=(*ACCENT, 60))
glow = glow.filter(ImageFilter.GaussianBlur(120))
img = Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")

glow2 = Image.new("RGBA", (W, H), (0,0,0,0))
gd2 = ImageDraw.Draw(glow2)
gd2.ellipse((W-700, H-700, W+300, H+300), fill=(*COOL, 50))
glow2 = glow2.filter(ImageFilter.GaussianBlur(180))
img = Image.alpha_composite(img.convert("RGBA"), glow2).convert("RGB")

out = sys.argv[1] if len(sys.argv) > 1 else "initrd_root/etc/wallpaper.bmp"
os.makedirs(os.path.dirname(out), exist_ok=True)
img.save(out, "BMP")
print("wrote", out, img.size)
