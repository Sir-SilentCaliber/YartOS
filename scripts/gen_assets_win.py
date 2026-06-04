#!/usr/bin/env python3
"""
Yart OS - Generate real raster assets from Windows UI assets.
Replaces procedural icons with authentic Windows 11 icons, cursor, and wallpaper.
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

def load_icon(dll, num, size=(ICON_W, ICON_H)):
    path = ico_path(dll, num)
    if not os.path.exists(path):
        print(f"  WARNING: {path} not found, generating fallback")
        return fallback_icon()
    img = Image.open(path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    img = img.resize(size, Image.LANCZOS)
    return img

def fallback_icon():
    img = Image.new("RGBA", (ICON_W, ICON_H), (0,0,0,0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2,2,29,29), radius=4, fill=(80,80,80,255), outline=(200,200,200,255), width=1)
    return img

# ── icon mapping ───────────────────────────────────────────────────────────
ICONS = [
    # Original app/system icons
    ("FILES",       "shell32.dll.mun",   3   ),   # folder
    ("TERM",        "imageres.dll.mun",  112 ),   # Windows Terminal
    ("EDITOR",      "imageres.dll.mun",  113 ),   # Notepad
    ("CLOCK",       "imageres.dll.mun",  5363),   # clock
    ("INFO",        "imageres.dll.mun",  5308),   # info
    ("FOLDER",      "shell32.dll.mun",   3   ),   # closed folder
    ("FILE",        "shell32.dll.mun",   1   ),   # generic document
    ("HOME",        "imageres.dll.mun",  30  ),   # Users / Home
    ("CONFIG",      "imageres.dll.mun",  5314),   # Settings gear
    ("DRAWER",      "imageres.dll.mun",  5360),   # File Explorer
    ("CALC",        "imageres.dll.mun",  109 ),   # Calculator
    ("MONITOR",     "imageres.dll.mun",  124 ),   # Resource Monitor

    # File-type icons
    ("IMG",         "imageres.dll.mun",  31  ),   # Pictures library
    ("VIDEO",       "imageres.dll.mun",  33  ),   # Videos library
    ("CODE",        "imageres.dll.mun",  1001),   # Windows logo / code
    ("MUSIC",       "imageres.dll.mun",  32  ),   # Music library
    ("ARCHIVE",     "imageres.dll.mun",  5406),   # archive-ish
    ("TEXT",        "imageres.dll.mun",  5304),   # text / document

    # Folder variant icons
    ("FOLDER_OPEN", "shell32.dll.mun",   4   ),   # open folder
    ("FOLDER_PIC",  "imageres.dll.mun",  5305),   # pictures folder
    ("FOLDER_MUSIC","imageres.dll.mun",  5306),   # music folder
    ("FOLDER_VIDEO","imageres.dll.mun",  5307),   # videos folder
    ("FOLDER_DOC",  "imageres.dll.mun",  5308),   # documents folder
    ("FOLDER_DL",   "imageres.dll.mun",  35  ),   # downloads folder

    # System icons
    ("RECYCLE",     "imageres.dll.mun",  74  ),   # recycle bin
    ("NETWORK",     "imageres.dll.mun",  5368),   # network
    ("LOCK",        "imageres.dll.mun",  6   ),   # lock icon
]

# ── cursor ─────────────────────────────────────────────────────────────────
CURSOR_W, CURSOR_H = 12, 18

def make_cursor():
    cur_path = os.path.join(WIN_CURSORS, "aero_arrow.cur")
    if not os.path.exists(cur_path):
        cur_path = os.path.join(WIN_CURSORS, "aero_arrow_l.cur")
    if os.path.exists(cur_path):
        img = Image.open(cur_path)
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        img = img.resize((CURSOR_W, CURSOR_H), Image.LANCZOS)
        return img
    img = Image.new("RGBA", (CURSOR_W, CURSOR_H), (0,0,0,0))
    d = ImageDraw.Draw(img)
    d.polygon([(0,0),(0,15),(4,11),(7,17),(9,17),(6,11),(11,11)],
              fill=(0,0,0,255))
    d.polygon([(1,2),(1,12),(4,9),(6,15),(7,15),(5,9),(9,9)],
              fill=(244,245,247,255))
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
            print(f"  Using wallpaper: {path}")
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
    print("  WARNING: No Windows wallpaper found")
    return Image.new("RGBA", (target_w, target_h), (27,31,36,255))

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

    print("Generating icons from Windows UI assets...")
    with open(os.path.join(BASE, "kernel", "gui", "asset_icons.c"), "w") as f:
        f.write("/* AUTO-GENERATED by scripts/gen_assets_win.py */\n")
        f.write('#include <yart/types.h>\n\n')
        for name, dll, num in ICONS:
            print(f"  {name:16s} <- {dll}/ICON{num}")
            img = load_icon(dll, num)
            emit_argb_array(f"icon_{name}", img, f)
        f.write("typedef struct { const u32 *px; int w; int h; } icon_asset_t;\n")
        f.write("const icon_asset_t yart_icons[] = {\n")
        for name, _, _ in ICONS:
            f.write(f"  {{ icon_{name}_pixels, icon_{name}_w, icon_{name}_h }},\n")
        f.write("};\n")
        f.write(f"const int yart_icons_count = {len(ICONS)};\n")
    print(f"  -> kernel/gui/asset_icons.c ({len(ICONS)} icons)")

    print("Generating cursor...")
    with open(os.path.join(BASE, "kernel", "gui", "asset_cursor.c"), "w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_cursor", make_cursor(), f)

    print("Generating wallpaper...")
    with open(os.path.join(BASE, "kernel", "gui", "asset_wallpaper.c"), "w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_wallpaper", make_wallpaper(), f)

    print("Done!")

if __name__ == "__main__":
    main()
