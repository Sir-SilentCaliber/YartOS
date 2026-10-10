#!/usr/bin/env python3
"""
Yart OS - Generate real raster assets from Windows UI assets.
Replaces procedural icons with authentic Windows 11 icons, cursor, and wallpaper.
Includes per-icon fallback vector generators if raw .ico files are missing.
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

# ── paths ──────────────────────────────────────────────────────────────────
BASE        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN_ASSETS  = os.path.join(os.path.dirname(BASE), "windows-ui-assets")
WIN_ICONS   = os.path.join(WIN_ASSETS, "Icons", "Windows 11", "ico")
WIN_CURSORS = os.path.join(WIN_ASSETS, "Cursors", "Windows 11")
WIN_WALL    = os.path.join(WIN_ASSETS, "Wallpapers", "Windows 11", "Desktop")

ICON_W, ICON_H = 32, 32

def ico_path(dll, num):
    return os.path.join(WIN_ICONS, dll, f"ICON{num}_1.ico")

def load_icon(dll, num, name=None, size=(ICON_W, ICON_H)):
    path = ico_path(dll, num)
    if not os.path.exists(path):
        return fallback_icon(name)
    try:
        img = Image.open(path)
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        return img.resize(size, Image.LANCZOS)
    except Exception:
        return fallback_icon(name)

# ── palette for fallbacks ──
ACCENT     = (232, 168, 124, 255)
ACCENT_DIM = (176, 122, 85,  255)
PANEL      = (31,  36,  44,  255)
PANEL_HI   = (39,  45,  55,  255)
TEXT       = (230, 232, 238, 255)
TEXT_DIM   = (139, 147, 160, 255)
TEXT_MUT   = (90,  98,  110, 255)
OK         = (143, 188, 143, 255)
WARN       = (224, 192, 136, 255)
ERR        = (204, 119, 119, 255)
BLK        = (10,  12,  16,  255)
DGREY      = (60,  66,  76,  255)

def new_icon():
    return Image.new("RGBA", (ICON_W, ICON_H), (0,0,0,0))

def rrect(d, box, r, fill, outline=None, w=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=w)

def fallback_icon(name=None):
    img = new_icon(); d = ImageDraw.Draw(img)
    if name == "FILES" or name == "FOLDER":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (3,11,29,15), 1, fill=(255,210,170,255))
    elif name == "FOLDER_OPEN":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,27,24), 2, fill=ACCENT_DIM, outline=BLK)
        d.polygon([(1,28),(6,14),(30,14),(25,28)], fill=ACCENT, outline=BLK)
    elif name == "FOLDER_PIC" or name == "IMG":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (16,14,28,26), 2, fill=(45, 120, 190, 255), outline=BLK)
        d.ellipse((18,16,21,19), fill=WARN)
        d.polygon([(17,24),(21,19),(26,24)], fill=(120, 200, 120, 255))
    elif name == "FOLDER_MUSIC" or name == "MUSIC":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (16,14,28,26), 2, fill=(140, 70, 180, 255), outline=BLK)
        d.ellipse((18,21,22,25), fill=TEXT)
        d.line((22,23,22,16), fill=TEXT, width=1)
    elif name == "FOLDER_VIDEO" or name == "VIDEO":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (16,14,28,26), 2, fill=(180, 50, 60, 255), outline=BLK)
        d.polygon([(20,17),(26,20),(20,23)], fill=TEXT)
    elif name == "FOLDER_DOC" or name == "TEXT" or name == "FILE":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (16,14,28,26), 2, fill=TEXT, outline=BLK)
        for y in (17, 20, 23): d.line((18, y, 26, y), fill=TEXT_MUT, width=1)
    elif name == "FOLDER_DL":
        d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
        rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
        rrect(d, (16,14,28,26), 2, fill=OK, outline=BLK)
        d.line((22,16,22,22), fill=BLK, width=2)
        d.polygon([(19,20),(22,24),(25,20)], fill=BLK)
    elif name == "RECYCLE":
        rrect(d, (7,9,24,27), 2, fill=DGREY, outline=BLK, w=1)
        rrect(d, (5,6,26,9), 1, fill=PANEL_HI, outline=BLK)
        d.rectangle((13,4,18,6), fill=PANEL_HI, outline=BLK)
        for x in (11, 15, 19): d.line((x, 12, x, 24), fill=OK, width=2)
    elif name == "TERM":
        rrect(d, (3,5,28,27), 3, fill=BLK, outline=PANEL_HI, w=1)
        rrect(d, (3,5,28,9), 3, fill=PANEL_HI)
        d.ellipse((6,6,8,8), fill=ERR); d.ellipse((10,6,12,8), fill=WARN); d.ellipse((14,6,16,8), fill=OK)
        d.line((7,14,11,17), fill=ACCENT, width=2); d.line((7,20,11,17), fill=ACCENT, width=2)
    else:
        rrect(d, (3,3,28,28), 4, fill=PANEL_HI, outline=ACCENT, w=1)
    return img

# ── icon mapping ───────────────────────────────────────────────────────────
ICONS = [
    # Original app/system icons
    ("FILES",       "shell32.dll.mun",   3   ),   # closed folder
    ("TERM",        "imageres.dll.mun",  112 ),   # terminal
    ("EDITOR",      "imageres.dll.mun",  113 ),   # notepad
    ("CLOCK",       "imageres.dll.mun",  5363),   # clock
    ("INFO",        "imageres.dll.mun",  5308),   # info
    ("FOLDER",      "shell32.dll.mun",   3   ),   # closed folder
    ("FILE",        "shell32.dll.mun",   1   ),   # generic document
    ("HOME",        "imageres.dll.mun",  123 ),   # Users / Home
    ("CONFIG",      "imageres.dll.mun",  5314),   # Settings gear
    ("DRAWER",      "imageres.dll.mun",  5360),   # File Explorer
    ("CALC",        "imageres.dll.mun",  109 ),   # Calculator
    ("MONITOR",     "imageres.dll.mun",  124 ),   # Resource Monitor

    # File-type icons
    ("IMG",         "imageres.dll.mun",  67  ),   # Pictures library
    ("VIDEO",       "imageres.dll.mun",  189 ),   # Videos library
    ("CODE",        "imageres.dll.mun",  1001),   # Windows code / file
    ("MUSIC",       "imageres.dll.mun",  108 ),   # Music library
    ("ARCHIVE",     "imageres.dll.mun",  165 ),   # Zip archive
    ("TEXT",        "imageres.dll.mun",  102 ),   # Text document

    # Folder variant icons
    ("FOLDER_OPEN", "shell32.dll.mun",   4   ),   # open folder
    ("FOLDER_PIC",  "imageres.dll.mun",  113 ),   # pictures folder (Windows 11)
    ("FOLDER_MUSIC","imageres.dll.mun",  108 ),   # music folder (Windows 11)
    ("FOLDER_VIDEO","imageres.dll.mun",  189 ),   # videos folder (Windows 11)
    ("FOLDER_DOC",  "imageres.dll.mun",  112 ),   # documents folder (Windows 11)
    ("FOLDER_DL",   "imageres.dll.mun",  184 ),   # downloads folder (Windows 11)

    # System icons
    ("RECYCLE",     "shell32.dll.mun",   32  ),   # recycle bin (Windows 11)
    ("NETWORK",     "imageres.dll.mun",  114 ),   # network
    ("LOCK",        "imageres.dll.mun",  59  ),   # lock icon
]

# ── cursor ─────────────────────────────────────────────────────────────────
CURSOR_W, CURSOR_H = 12, 18

def make_cursor():
    cur_path = os.path.join(WIN_CURSORS, "aero_arrow.cur")
    if not os.path.exists(cur_path):
        cur_path = os.path.join(WIN_CURSORS, "aero_arrow_l.cur")
    if os.path.exists(cur_path):
        try:
            img = Image.open(cur_path)
            if img.mode != "RGBA": img = img.convert("RGBA")
            return img.resize((CURSOR_W, CURSOR_H), Image.LANCZOS)
        except Exception: pass
    img = Image.new("RGBA", (CURSOR_W, CURSOR_H), (0,0,0,0))
    d = ImageDraw.Draw(img)
    d.polygon([(0,0),(0,15),(4,11),(7,17),(9,17),(6,11),(11,11)], fill=(0,0,0,255))
    d.polygon([(1,2),(1,12),(4,9),(6,15),(7,15),(5,9),(9,9)], fill=(244,245,247,255))
    return img

# ── wallpaper ──────────────────────────────────────────────────────────────
def make_wallpaper(target_w=480, target_h=300):
    candidates = [
        os.path.join(WIN_WALL, "Windows", "img0.jpg"),
        os.path.join(WIN_WALL, "Sunrise", "img28.jpg"),
        os.path.join(WIN_WALL, "Glow", "img20.jpg"),
    ]
    for path in candidates:
        if os.path.exists(path):
            try:
                img = Image.open(path).convert("RGBA")
                orig_w, orig_h = img.size
                target_ratio = target_w / target_h
                orig_ratio = orig_w / orig_h
                if orig_ratio > target_ratio:
                    new_w = int(orig_h * target_ratio)
                    left = (orig_w - new_w) // 2
                    img = img.crop((left, 0, left + new_w, orig_h))
                else:
                    new_h = int(orig_w / target_ratio)
                    top = (orig_h - new_h) // 2
                    img = img.crop((0, top, orig_w, top + new_h))
                return img.resize((target_w, target_h), Image.LANCZOS)
            except Exception: pass
    img = Image.new("RGBA", (target_w, target_h), (17,20,26,255))
    px = img.load()
    for y in range(target_h):
        t = y / (target_h - 1)
        r = int(27 + (17 - 27) * t)
        g = int(31 + (20 - 31) * t)
        b = int(36 + (26 - 36) * t)
        for x in range(target_w): px[x, y] = (r, g, b, 255)
    return img

# ── emit ───────────────────────────────────────────────────────────────────
def emit_argb_array(name, img, f):
    px = img.load()
    w, h = img.size
    f.write(f"const u32 {name}_pixels[{w*h}] = {{\n  ")
    n = 0
    for y in range(h):
        for x in range(w):
            r,g,b,a = px[x,y]
            v = (a<<24) | (r<<16) | (g<<8) | b
            f.write(f"0x{v:08X},")
            n += 1
            if n % 12 == 0:
                f.write("\n  ")
    f.write("\n};\n")
    f.write(f"const int {name}_w = {w};\nconst int {name}_h = {h};\n\n")

def main():
    os.makedirs(os.path.join(BASE, "kernel", "gui"), exist_ok=True)
    with open(os.path.join(BASE, "kernel", "gui", "asset_icons.c"), "w") as f:
        f.write("/* AUTO-GENERATED by scripts/gen_assets_win.py */\n")
        f.write('#include <yart/types.h>\n\n')
        for name, dll, num in ICONS:
            img = load_icon(dll, num, name=name)
            emit_argb_array(f"icon_{name}", img, f)
        f.write("typedef struct { const u32 *px; int w; int h; } icon_asset_t;\n")
        f.write("const icon_asset_t yart_icons[] = {\n")
        for name, _, _ in ICONS:
            f.write(f"  {{ icon_{name}_pixels, icon_{name}_w, icon_{name}_h }},\n")
        f.write("};\n")
        f.write(f"const int yart_icons_count = {len(ICONS)};\n")

    with open(os.path.join(BASE, "kernel", "gui", "asset_cursor.c"), "w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_cursor", make_cursor(), f)

    with open(os.path.join(BASE, "kernel", "gui", "asset_wallpaper.c"), "w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_wallpaper", make_wallpaper(), f)

    print("Generated Windows asset tables cleanly.")

if __name__ == "__main__":
    main()
