#!/usr/bin/env python3
"""
Yart OS - generate real raster assets (icons, cursor, wallpaper) and emit
them as C source files containing raw ARGB byte arrays.

Output:
  kernel/gui/asset_icons.c      - icon ARGB tables (24x24 each)
  kernel/gui/asset_cursor.c     - 12x18 cursor
  kernel/gui/asset_wallpaper.c  - wallpaper tile

Each pixel is one 32-bit ARGB value: alpha=0 means transparent.
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

# palette
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
BG_TOP     = (27,  31,  36,  255)
BG_BOT     = (17,  20,  26,  255)
DOT        = (44,  50,  62,  255)

ICON_W, ICON_H = 32, 32

def new_icon():
    return Image.new("RGBA", (ICON_W, ICON_H), (0,0,0,0))

def rrect(d, box, r, fill, outline=None, w=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=w)

# ---------- icons ----------
def icon_files():
    img = new_icon(); d = ImageDraw.Draw(img)
    # back folder
    rrect(d, (3,9,24,28), 2, fill=ACCENT_DIM, outline=BLK, w=1)
    # tab on top
    d.polygon([(3,9),(11,5),(17,5),(17,9)], fill=ACCENT, outline=BLK)
    # front folder (offset)
    rrect(d, (7,12,28,29), 2, fill=ACCENT, outline=BLK, w=1)
    rrect(d, (7,13,28,16), 1, fill=(255,210,170,255))
    return img

def icon_terminal():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (3,5,28,27), 3, fill=BLK, outline=PANEL_HI, w=1)
    rrect(d, (3,5,28,9), 3, fill=PANEL_HI)
    # 3 dots
    d.ellipse((6,6,8,8),  fill=ERR)
    d.ellipse((10,6,12,8), fill=WARN)
    d.ellipse((14,6,16,8), fill=OK)
    # ">" prompt
    d.line((7,14,11,17), fill=ACCENT, width=2)
    d.line((7,20,11,17), fill=ACCENT, width=2)
    # underscore
    d.line((13,21,21,21), fill=ACCENT, width=2)
    return img

def icon_editor():
    img = new_icon(); d = ImageDraw.Draw(img)
    # paper
    rrect(d, (4,4,23,28), 1, fill=TEXT, outline=BLK, w=1)
    # text lines
    for y in (9, 13, 17, 21):
        d.line((7, y, 20, y), fill=TEXT_MUT, width=1)
    # pencil overlay (top right corner sticks out)
    d.polygon([(17,21),(26,12),(29,15),(20,24)], fill=ACCENT, outline=BLK)
    d.polygon([(17,21),(15,24),(20,24)], fill=BLK)
    d.polygon([(26,12),(29,15),(28,11),(25,12)], fill=DGREY, outline=BLK)
    return img

def icon_clock():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.ellipse((2,2,29,29), fill=PANEL_HI, outline=BLK, width=1)
    d.ellipse((4,4,27,27), fill=PANEL, outline=None)
    # ticks
    for ang in range(0, 360, 30):
        x = 15.5 + 11 * math.cos(math.radians(ang - 90))
        y = 15.5 + 11 * math.sin(math.radians(ang - 90))
        d.ellipse((x-1,y-1,x+1,y+1), fill=TEXT)
    # hands
    d.line((15.5,15.5, 15.5, 8), fill=TEXT, width=2)
    d.line((15.5,15.5, 22, 18), fill=ACCENT, width=2)
    d.ellipse((14,14,17,17), fill=ACCENT, outline=BLK)
    return img

def icon_info():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.ellipse((2,2,29,29), fill=ACCENT_DIM, outline=BLK, width=1)
    d.ellipse((4,4,27,27), fill=ACCENT)
    d.ellipse((13,7,17,11),  fill=BLK)
    d.rectangle((13,13,17,24), fill=BLK)
    return img

def icon_folder():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
    rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
    rrect(d, (3,11,29,15), 1, fill=(255,210,170,255))
    return img

def icon_file():
    img = new_icon(); d = ImageDraw.Draw(img)
    # paper with corner fold
    d.polygon([(7,4),(20,4),(27,11),(27,28),(7,28)],
              fill=TEXT, outline=BLK)
    # corner fold
    d.polygon([(20,4),(27,11),(20,11)], fill=TEXT_DIM, outline=BLK)
    # text lines
    for y in (15, 19, 23):
        d.line((10, y, 24, y), fill=TEXT_MUT, width=1)
    return img

def icon_home():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.polygon([(2,16),(15.5,3),(29,16)], fill=ACCENT, outline=BLK)
    d.rectangle((6,16,25,28), fill=PANEL_HI, outline=BLK, width=1)
    d.rectangle((13,18,18,28), fill=ACCENT_DIM, outline=BLK)
    d.rectangle((8,18,11,21), fill=TEXT)
    d.rectangle((20,18,23,21), fill=TEXT)
    return img

def icon_config():
    img = new_icon(); d = ImageDraw.Draw(img)
    cx_, cy_ = 15.5, 15.5
    pts = []
    for i in range(16):
        ang = math.radians(i * 22.5)
        r = 13 if (i % 2 == 0) else 9
        pts.append((cx_ + r*math.cos(ang), cy_ + r*math.sin(ang)))
    d.polygon(pts, fill=ACCENT_DIM, outline=BLK)
    d.ellipse((6,6,25,25), fill=ACCENT, outline=BLK)
    d.ellipse((11,11,20,20), fill=PANEL, outline=BLK)
    return img

def icon_drawer():
    img = new_icon(); d = ImageDraw.Draw(img)
    for r in range(3):
        for c in range(3):
            x = 5 + c*9
            y = 5 + r*9
            d.ellipse((x,y,x+5,y+5), fill=ACCENT)
    return img

def icon_calc():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (4,3,27,28), 3, fill=PANEL_HI, outline=BLK, w=1)
    # display
    rrect(d, (6,6,25,11), 1, fill=BLK, outline=PANEL)
    d.line((19, 8, 23, 8), fill=ACCENT, width=2)
    # button grid (4 cols x 4 rows)
    bw, bh = 4, 3
    gap = 1
    x0, y0 = 6, 13
    for r in range(4):
        for c in range(4):
            x = x0 + c*(bw+gap)
            y = y0 + r*(bh+gap)
            color = ACCENT if (c==3) else (TEXT_DIM if r==0 else TEXT)
            rrect(d, (x, y, x+bw, y+bh), 1, fill=color)
    return img

def icon_monitor():
    img = new_icon(); d = ImageDraw.Draw(img)
    # screen
    rrect(d, (2,4,29,22), 2, fill=BLK, outline=PANEL_HI, w=1)
    # graph bars
    bars = [4, 7, 5, 9, 11, 8, 13, 10, 14, 12, 9, 15, 11, 13, 8, 12]
    for i, h in enumerate(bars):
        x = 4 + i*1.5
        d.line((x, 20, x, 20-h), fill=ACCENT, width=1)
    # baseline
    d.line((3, 20, 28, 20), fill=ACCENT_DIM, width=1)
    # stand
    d.rectangle((13,22,18,26), fill=PANEL_HI, outline=BLK)
    d.line((7,28,24,28), fill=PANEL_HI, width=2)
    return img

# --- file-type icons (12-17) ---
def icon_img():

    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (4,4,27,27), 3, fill=(45,120,190,255), outline=BLK, w=1)
    d.ellipse((8,8,13,13), fill=WARN)
    d.polygon([(6,24),(14,15),(21,24)], fill=(120,200,120,255), outline=BLK)
    d.polygon([(15,24),(21,17),(26,24)], fill=(80,160,90,255), outline=BLK)
    return img

def icon_video():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (3,5,28,26), 3, fill=(180,50,60,255), outline=BLK, w=1)
    d.polygon([(12,10),(22,15),(12,21)], fill=TEXT, outline=BLK)
    for y in (7, 12, 17, 22):
        d.rectangle((4,y,6,y+2), fill=TEXT)
        d.rectangle((25,y,27,y+2), fill=TEXT)
    return img

def icon_code():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (4,4,27,27), 2, fill=PANEL_HI, outline=BLK, w=1)
    d.line((11,10,6,15), fill=ACCENT, width=2)
    d.line((6,15,11,21), fill=ACCENT, width=2)
    d.line((20,10,25,15), fill=ACCENT, width=2)
    d.line((25,15,20,21), fill=ACCENT, width=2)
    d.line((17,9,14,22), fill=TEXT_DIM, width=2)
    return img

def icon_music():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (4,4,27,27), 3, fill=(140,70,180,255), outline=BLK, w=1)
    d.ellipse((7,19,13,24), fill=TEXT, outline=BLK)
    d.ellipse((17,17,23,22), fill=TEXT, outline=BLK)
    d.line((13,21,13,9), fill=TEXT, width=2)
    d.line((23,19,23,7), fill=TEXT, width=2)
    d.polygon([(13,9),(23,7),(23,11),(13,13)], fill=TEXT)
    return img

def icon_archive():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (5,6,26,27), 2, fill=(210,160,50,255), outline=BLK, w=1)
    for y in range(8, 22, 4):
        d.rectangle((14,y,17,y+2), fill=DGREY, outline=BLK)
    rrect(d, (13,22,18,26), 1, fill=TEXT, outline=BLK)
    return img

def icon_text():
    return icon_file()

# --- folder variants (18-23) ---
def icon_folder_open():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
    rrect(d, (3,10,27,24), 2, fill=ACCENT_DIM, outline=BLK)
    d.polygon([(1,28),(6,14),(30,14),(25,28)], fill=ACCENT, outline=BLK)
    return img

def _folder_badge(badge_fn):
    img = new_icon(); d = ImageDraw.Draw(img)
    d.polygon([(3,11),(11,7),(17,7),(17,11)], fill=ACCENT_DIM, outline=BLK)
    rrect(d, (3,10,29,27), 3, fill=ACCENT, outline=BLK, w=1)
    rrect(d, (3,11,29,15), 1, fill=(255,210,170,255))
    badge_fn(d)
    return img

def icon_folder_pic():
    def b(d):
        rrect(d, (16,14,28,26), 2, fill=(45,120,190,255), outline=BLK)
        d.ellipse((18,16,21,19), fill=WARN)
        d.polygon([(17,24),(21,19),(26,24)], fill=(120,200,120,255))
    return _folder_badge(b)

def icon_folder_music():
    def b(d):
        rrect(d, (16,14,28,26), 2, fill=(140,70,180,255), outline=BLK)
        d.ellipse((18,21,22,25), fill=TEXT)
        d.line((22,23,22,16), fill=TEXT, width=1)
        d.line((22,16,26,15), fill=TEXT, width=1)
    return _folder_badge(b)

def icon_folder_video():
    def b(d):
        rrect(d, (16,14,28,26), 2, fill=(180,50,60,255), outline=BLK)
        d.polygon([(20,17),(26,20),(20,23)], fill=TEXT)
    return _folder_badge(b)

def icon_folder_doc():
    def b(d):
        rrect(d, (16,14,28,26), 2, fill=TEXT, outline=BLK)
        for y in (17, 20, 23):
            d.line((18,y,26,y), fill=TEXT_MUT, width=1)
    return _folder_badge(b)

def icon_folder_dl():
    def b(d):
        rrect(d, (16,14,28,26), 2, fill=OK, outline=BLK)
        d.line((22,16,22,22), fill=BLK, width=2)
        d.polygon([(19,20),(22,24),(25,20)], fill=BLK)
    return _folder_badge(b)

# --- system icons (24-26) ---
def icon_recycle():
    img = new_icon(); d = ImageDraw.Draw(img)
    rrect(d, (7,9,24,27), 2, fill=DGREY, outline=BLK, w=1)
    rrect(d, (5,6,26,9), 1, fill=PANEL_HI, outline=BLK)
    d.rectangle((13,4,18,6), fill=PANEL_HI, outline=BLK)
    for x in (11, 15, 19):
        d.line((x,12,x,24), fill=OK, width=2)
    return img

def icon_network():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.ellipse((4,4,27,27), fill=(40,140,220,255), outline=BLK, width=1)
    d.ellipse((9,4,22,27), fill=(0,0,0,0), outline=TEXT, width=1)
    d.line((4,15,27,15), fill=TEXT, width=1)
    d.line((6,10,25,10), fill=TEXT, width=1)
    d.line((6,21,25,21), fill=TEXT, width=1)
    return img

def icon_lock():
    img = new_icon(); d = ImageDraw.Draw(img)
    d.arc((9,4,22,18), start=180, end=0, fill=WARN, width=3)
    rrect(d, (6,13,25,27), 3, fill=WARN, outline=BLK, w=1)
    d.ellipse((14,17,17,20), fill=BLK)
    d.line((15,19,15,23), fill=BLK, width=2)
    return img

ICONS = [
    # 0-11: app/system
    ("FILES",        icon_files),
    ("TERM",         icon_terminal),
    ("EDITOR",       icon_editor),
    ("CLOCK",        icon_clock),
    ("INFO",         icon_info),
    ("FOLDER",       icon_folder),
    ("FILE",         icon_file),
    ("HOME",         icon_home),
    ("CONFIG",       icon_config),
    ("DRAWER",       icon_drawer),
    ("CALC",         icon_calc),
    ("MONITOR",      icon_monitor),
    # 12-17: file types
    ("IMG",          icon_img),
    ("VIDEO",        icon_video),
    ("CODE",         icon_code),
    ("MUSIC",        icon_music),
    ("ARCHIVE",      icon_archive),
    ("TEXT",         icon_text),
    # 18-23: folder variants
    ("FOLDER_OPEN",  icon_folder_open),
    ("FOLDER_PIC",   icon_folder_pic),
    ("FOLDER_MUSIC", icon_folder_music),
    ("FOLDER_VIDEO", icon_folder_video),
    ("FOLDER_DOC",   icon_folder_doc),
    ("FOLDER_DL",    icon_folder_dl),
    # 24-26: system
    ("RECYCLE",      icon_recycle),
    ("NETWORK",      icon_network),
    ("LOCK",         icon_lock),
]


# ---------- cursor ----------
CURSOR_W, CURSOR_H = 12, 18
def make_cursor():
    img = Image.new("RGBA", (CURSOR_W, CURSOR_H), (0,0,0,0))
    d = ImageDraw.Draw(img)
    # outline shadow
    d.polygon([(0,0),(0,15),(4,11),(7,17),(9,17),(6,11),(11,11)],
              fill=(0,0,0,255))
    # white fill
    d.polygon([(1,2),(1,12),(4,9),(6,15),(7,15),(5,9),(9,9)],
              fill=(244,245,247,255))
    return img

# ---------- wallpaper ----------
def make_wallpaper(w=480, h=300):
    img = Image.new("RGBA", (w, h), BG_BOT)
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        r = int(BG_TOP[0] + (BG_BOT[0]-BG_TOP[0]) * t)
        g = int(BG_TOP[1] + (BG_BOT[1]-BG_TOP[1]) * t)
        b = int(BG_TOP[2] + (BG_BOT[2]-BG_TOP[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b, 255)
    # subtle dot grid every 32 px
    d = ImageDraw.Draw(img)
    for y in range(32, h, 32):
        for x in range(32, w, 32):
            d.point((x, y), fill=DOT)
    # warm corner glow (top-left)
    glow = Image.new("RGBA", (w, h), (0,0,0,0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((-120, -120, 360, 360), fill=(232, 168, 124, 50))
    glow = glow.filter(ImageFilter.GaussianBlur(60))
    img = Image.alpha_composite(img, glow)
    # cool counter-glow (bottom-right)
    glow2 = Image.new("RGBA", (w, h), (0,0,0,0))
    gd2 = ImageDraw.Draw(glow2)
    gd2.ellipse((w-360, h-360, w+120, h+120), fill=(80, 110, 160, 30))
    glow2 = glow2.filter(ImageFilter.GaussianBlur(80))
    img = Image.alpha_composite(img, glow2)
    return img

# ---------- emit ----------
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
    os.makedirs("kernel/gui", exist_ok=True)
    with open("kernel/gui/asset_icons.c","w") as f:
        f.write("/* AUTO-GENERATED by scripts/gen_assets.py */\n")
        f.write('#include <yart/types.h>\n\n')
        for name, fn in ICONS:
            img = fn()
            emit_argb_array(f"icon_{name}", img, f)
        f.write("typedef struct { const u32 *px; int w; int h; } icon_asset_t;\n")
        f.write("const icon_asset_t yart_icons[] = {\n")
        for name, _ in ICONS:
            f.write(f"  {{ icon_{name}_pixels, icon_{name}_w, icon_{name}_h }},\n")
        f.write("};\n")
        f.write(f"const int yart_icons_count = {len(ICONS)};\n")
    with open("kernel/gui/asset_cursor.c","w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_cursor", make_cursor(), f)
    with open("kernel/gui/asset_wallpaper.c","w") as f:
        f.write("/* AUTO-GENERATED */\n#include <yart/types.h>\n\n")
        emit_argb_array("yart_wallpaper", make_wallpaper(), f)
    print("Generated asset files:",
          "icons %dx%d, cursor %dx%d, wallpaper rendered" %
          (ICON_W, ICON_H, CURSOR_W, CURSOR_H))

if __name__ == "__main__":
    main()
