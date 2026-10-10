#!/usr/bin/env python3
"""Render the wallpaper into a real 24-bit BMP that gets bundled into
   /etc/wallpaper.bmp on the initrd."""
import os, sys, math
try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.stderr.write("\nERROR: Pillow not found. Install with:\n  pip3 install Pillow\n  or: sudo apt install python3-pil\n\n")
    sys.exit(1)

W, H = 1280, 800

BASE        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KORA_WALL   = os.path.join(BASE, "kora", "wallpapers", "default.png")
WIN_WALL    = os.path.join(os.path.dirname(BASE), "windows-ui-assets", "Wallpapers", "Windows 11", "Desktop")

# Try our Kora wallpaper first, then Windows 11 "Bloom", then fallback
candidates = [
    KORA_WALL,
    os.path.join(WIN_WALL, "Windows", "img0.jpg"),
    os.path.join(WIN_WALL, "Sunrise", "img28.jpg"),
    os.path.join(WIN_WALL, "Glow", "img20.jpg"),
    os.path.join(WIN_WALL, "Flow", "img32.jpg"),
    os.path.join(WIN_WALL, "Captured Motion", "img24.jpg"),
]

img = None
for path in candidates:
    if os.path.exists(path):
        print(f"wallpaper: using {path}")
        img = Image.open(path).convert("RGB")
        # Center-crop to match target aspect ratio
        orig_w, orig_h = img.size
        target_ratio = W / H
        orig_ratio = orig_w / orig_h
        if orig_ratio > target_ratio:
            new_w = int(orig_h * target_ratio)
            left = (orig_w - new_w) // 2
            img = img.crop((left, 0, left + new_w, orig_h))
        else:
            new_h = int(orig_w / target_ratio)
            top = (orig_h - new_h) // 2
            img = img.crop((0, top, orig_w, top + new_h))
        img = img.resize((W, H), Image.LANCZOS)
        break

if img is None:
    # Fallback: generate gradient
    BG_TOP = (27, 31, 36)
    BG_BOT = (17, 20, 26)
    DOT    = (44, 50, 62)
    ACCENT = (232, 168, 124)
    COOL   = (80, 110, 160)
    print("wallpaper: no Windows wallpaper found, generating fallback gradient")
    img = Image.new("RGB", (W, H), BG_BOT)
    px = img.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(BG_TOP[0] + (BG_BOT[0]-BG_TOP[0]) * t)
        g = int(BG_TOP[1] + (BG_BOT[1]-BG_TOP[1]) * t)
        b = int(BG_TOP[2] + (BG_BOT[2]-BG_TOP[2]) * t)
        for x in range(W):
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(img)
    for y in range(48, H, 48):
        for x in range(48, W, 48):
            d.point((x, y), fill=DOT)
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
