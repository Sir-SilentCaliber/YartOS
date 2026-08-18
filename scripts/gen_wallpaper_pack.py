#!/usr/bin/env python3
"""Generate Yart OS wallpaper pack (build/wallpaper.bin).

Produces a set of calm, enterprise-grade wallpapers procedurally, writes
each as a PNG to kora/wallpapers/, then packs them all into a single
binary blob that the compositor links at build time.

Binary format (little-endian):
  u8  magic[5] = "YWALL"
  u16 version = 2
  u16 count           -- number of wallpapers
  u8  reserved[8]
  u32 offs[count]     -- byte offset to each wallpaper entry
  ... entries ...
  Each entry:
    u16 w, h
    u8  reserved[4]
    u8  pixels[w*h*4]  (BGRA, matches the compositor's ARGB() layout)

The compositor will default to index 0 (twilight-dunes) and cycle on hotkey.
"""
import sys, os
try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.stderr.write("\nERROR: Pillow not found. Install with:\n  pip3 install Pillow\n  or: sudo apt install python3-pil\n\n")
    sys.exit(1)
import math, random

W,H = 1280,800
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WP_DIR = os.path.join(ROOT, "kora", "wallpapers")
OUT_BIN = os.path.join(ROOT, "build", "wallpaper.bin")
os.makedirs(WP_DIR, exist_ok=True)
os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)

# ---------- helpers ----------
def ridge(y_base, wavelist, step=2):
    pts=[]
    for x in range(0, W+step, step):
        y = float(y_base)
        for wlen, amp, phase in wavelist:
            y += amp*math.sin(x*(2*math.pi/wlen) + phase)
        pts.append((x, int(y)))
    return pts

def sky_gradient(top_rgb, bot_rgb):
    img = Image.new("RGB", (W,H))
    px = img.load()
    tr,tg,tb = top_rgb
    br,bg,bb = bot_rgb
    for y in range(H):
        t = y/H
        r = int(tr + (br-tr)*t)
        g = int(tg + (bg-tg)*t)
        b = int(tb + (bb-tb)*t)
        for x in range(W): px[x,y] = (r,g,b)
    return img

def radial_glow(img, cx, cy, layers, blur=100):
    glow = Image.new("RGBA",(W,H),(0,0,0,0))
    d = ImageDraw.Draw(glow)
    for radius,alpha,col in layers:
        d.ellipse([cx-radius,cy-radius,cx+radius,cy+radius],fill=(*col,alpha))
    glow = glow.filter(ImageFilter.GaussianBlur(blur))
    return Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")

def haze_band(img, y0, y1, col=(225,225,230), max_a=22, blur=28):
    haze = Image.new("RGBA",(W,H),(0,0,0,0))
    hd = ImageDraw.Draw(haze)
    for yy in range(int(y0), int(y1)):
        t = (yy - y0)/(y1-y0)
        a = int(max_a*(1 - abs(2*t-1)))
        hd.line([(0,yy),(W,yy)], fill=(*col,a))
    haze = haze.filter(ImageFilter.GaussianBlur(blur))
    return Image.alpha_composite(img.convert("RGBA"), haze).convert("RGB")

def dune_layers(img, layers):
    for (y_base, waves, color, blur_r) in layers:
        layer = Image.new("RGB",(W,H), color)
        mask = Image.new("L",(W,H),0)
        md = ImageDraw.Draw(mask)
        pts = ridge(y_base, waves, step=2)
        poly = [(0,H)] + pts + [(W,H)]
        md.polygon(poly, fill=255)
        if blur_r > 0: mask = mask.filter(ImageFilter.GaussianBlur(blur_r))
        img.paste(layer, (0,0), mask)
    return img

def vignette(img, cx=None, cy=None, start=0.64, strength=36, blur=50):
    if cx is None: cx = W//2
    if cy is None: cy = int(H*0.56)
    vign = Image.new("RGBA",(W,H),(0,0,0,0))
    vd = ImageDraw.Draw(vign)
    maxr = math.hypot(W,H)*0.62
    for y in range(0,H,8):
        for x in range(0,W,8):
            d = math.hypot(x-cx,y-cy)/maxr
            if d > start:
                a = int(min(strength, (d-start)*100))
                vd.rectangle([x,y,x+7,y+7],fill=(0,0,0,a))
    vign = vign.filter(ImageFilter.GaussianBlur(blur))
    return Image.alpha_composite(img.convert("RGBA"), vign).convert("RGB")

def img_to_bgra(img):
    raw = bytearray()
    for y in range(H):
        for x in range(W):
            r,g,b = img.getpixel((x,y))[0:3]
            raw += bytes((b,g,r,255))
    return bytes(raw)

# ---------- wallpaper generators ----------

def wp_twilight_dunes(seed):
    """Improved twilight dunes - modern, crisp, better colors - keeps original vibe but better."""
    random.seed(seed)
    # Deeper, more modern gradient: dark slate to warm dusk
    img = sky_gradient((0x12,0x16,0x24),(0x3A,0x3D,0x4A))
    # Subtle aurora glow top
    img = radial_glow(img, int(W*0.75), int(H*0.35), [
        (700,10,(120,140,180)),(450,18,(180,160,200)),
        (300,28,(255,190,140)),(140,40,(255,210,170))], blur=90)
    # Warm ground glow
    fill = Image.new("RGBA",(W,H),(0,0,0,0))
    fd = ImageDraw.Draw(fill)
    fcx,fcy = int(W*0.2), int(H*0.25)
    fd.ellipse([fcx-400,fcy-300,fcx+400,fcy+300],fill=(90,110,160,12))
    fill = fill.filter(ImageFilter.GaussianBlur(100))
    img = Image.alpha_composite(img.convert("RGBA"), fill).convert("RGB")
    # Haze bands for depth
    img = haze_band(img, H*0.42, H*0.60, col=(190,195,205), max_a=18, blur=24)
    img = haze_band(img, H*0.55, H*0.70, col=(140,145,155), max_a=14, blur=20)
    # More detailed dune layers - 6 layers for depth, sharper
    img = dune_layers(img, [
        (int(H*0.48), [(1600,22,0.5),(700,9,1.8),(300,4,0.3)],             (100,105,115), 8.0),
        (int(H*0.54), [(1400,28,1.1),(600,11,2.4)],                        (78,82,90), 5.5),
        (int(H*0.61), [(1200,38,2.0),(520,13,0.4)],                        (58,62,70), 4.0),
        (int(H*0.68), [(1000,52,0.3),(440,14,1.2)],                        (42,46,54), 2.8),
        (int(H*0.77), [(850, 48,1.5),(360,11,3.0)],                        (28,32,40), 1.6),
        (int(H*0.88), [(1800,36,2.2),(520,6,1.0)],                          (18,20,26), 0.8),
    ])
    # Subtle grain for modern feel
    grain = Image.new("RGBA",(W,H),(0,0,0,0))
    gd = ImageDraw.Draw(grain)
    rng = random.Random(seed+10)
    for _ in range(8000):
        x=rng.randint(0,W-1); y=rng.randint(0,H-1)
        a=rng.randint(2,10)
        gd.point((x,y), fill=(255,255,255,a))
    grain = grain.filter(ImageFilter.GaussianBlur(0.3))
    img = Image.alpha_composite(img.convert("RGBA"), grain).convert("RGB")
    img = vignette(img, strength=32, blur=60)
    return img

def wp_midnight(seed):
    """Deep navy night sky with cool blue ambient glow."""
    random.seed(seed)
    img = sky_gradient((0x07,0x0A,0x18),(0x18,0x24,0x3D))
    # stars
    star_layer = Image.new("RGBA",(W,H),(0,0,0,0))
    sd = ImageDraw.Draw(star_layer)
    rng = random.Random(seed)
    for _ in range(350):
        x=rng.randint(0,W-1); y=rng.randint(0,int(H*0.5))
        sd.point((x,y), fill=(255,255,255,rng.randint(40,220)))
    star_layer = star_layer.filter(ImageFilter.GaussianBlur(0.4))
    img = Image.alpha_composite(img.convert("RGBA"), star_layer).convert("RGB")
    # moon glow
    img = radial_glow(img, int(W*0.78), int(H*0.28), [
        (200,22,(200,210,230)),(120,38,(220,225,240)),(60,80,(240,242,250)),], blur=60)
    img = haze_band(img, H*0.40, H*0.60, col=(90,110,150), max_a=14, blur=32)
    img = dune_layers(img, [
        (int(H*0.48), [(1400,24,1.1),(620,10,2.4)],             (40,48,70), 6.0),
        (int(H*0.58), [(1100,42,0.3),(480,10,1.6)],             (28,34,54), 4.0),
        (int(H*0.68), [(900, 56,2.2),(400,12,0.7)],             (18,22,38), 2.0),
        (int(H*0.78), [(780, 50,0.5),(340,10,3.0)],             (12,15,26), 1.0),
        (int(H*0.90), [(1400,40,1.8),(460,6,0.8)],              (7, 9, 16),  0.8),
    ])
    img = vignette(img, strength=42)
    return img

def wp_ocean_breeze(seed):
    """Cool teal/green sunrise over abstract wave layers."""
    random.seed(seed)
    img = sky_gradient((0x38,0x6B,0x7E),(0xE0,0xC7,0xA6))
    img = radial_glow(img, int(W*0.5), int(H*0.70), [
        (480,20,(255,200,150)),(260,32,(255,210,170)),
        (120,55,(255,225,195))], blur=100)
    img = haze_band(img, H*0.50, H*0.68, col=(210,220,230), max_a=20, blur=30)
    img = dune_layers(img, [
        (int(H*0.55), [(1600,22,0.2),(700,8,2.0)],              (70,110,120), 6.0),
        (int(H*0.64), [(1300,34,1.1),(560,10,0.6)],             (52,88,100), 4.0),
        (int(H*0.73), [(1000,46,2.6),(420,10,1.3)],             (38,70,84),  3.0),
        (int(H*0.82), [(800, 54,0.4),(360,10,3.1)],             (26,52,64),  2.0),
        (int(H*0.92), [(1500,40,2.1),(500,6,1.2)],              (16,32,40),  1.0),
    ])
    img = vignette(img)
    return img

def wp_graphite(seed):
    """Monochrome slate-gray enterprise look."""
    random.seed(seed)
    img = sky_gradient((0x33,0x35,0x38),(0xA8,0xAB,0xB0))
    img = radial_glow(img, int(W*0.20), int(H*0.40), [
        (400,10,(255,255,255)),(220,18,(240,240,245))], blur=120)
    img = haze_band(img, H*0.40, H*0.60, col=(200,200,205), max_a=16, blur=32)
    img = dune_layers(img, [
        (int(H*0.52), [(1400,20,0.7),(600,8,2.0)],              (110,113,118), 6.0),
        (int(H*0.62), [(1100,38,1.3),(500,10,0.2)],             (86, 89, 94),  4.0),
        (int(H*0.72), [(900, 50,2.4),(420,10,1.1)],             (64, 67, 72),  2.0),
        (int(H*0.82), [(780, 50,0.1),(340,10,3.0)],             (44, 47, 52),  1.0),
        (int(H*0.92), [(1500,36,1.7),(480,6,0.9)],              (28, 30, 34),  0.8),
    ])
    img = vignette(img, strength=28)
    return img

WALLPAPERS = [
    ("twilight-dunes", wp_twilight_dunes, 42),
    ("midnight",       wp_midnight,       17),
    ("ocean-breeze",   wp_ocean_breeze,   88),
    ("graphite",       wp_graphite,       23),
]

# ---------- main ----------
def main():
    blobs = []
    names = []
    # The Skift desktop's default wallpaper (deep indigo abstract), scaled to
    # the framebuffer. It is index 0 so the compositor boots with it.
    skift_path = os.path.join(WP_DIR, "skift_abstract.png")
    if os.path.exists(skift_path):
        img = Image.open(skift_path).convert("RGB").resize((W, H), Image.LANCZOS)
        img.save(os.path.join(WP_DIR, "abstract.png"), "PNG")
        blobs.append(img_to_bgra(img))
        names.append("abstract")
        print(f"  + abstract: {skift_path} -> {W}x{H}")
    for name, fn, seed in WALLPAPERS:
        img = fn(seed)
        png_path = os.path.join(WP_DIR, f"{name}.png")
        img.save(png_path, "PNG")
        blobs.append(img_to_bgra(img))
        names.append(name)
        print(f"  + {name}: {png_path}")

    # Always write default.png as a copy of the first wallpaper so old builds
    # that expect it don't break.
    if names:
        import shutil
        shutil.copyfile(os.path.join(WP_DIR, f"{names[0]}.png"),
                        os.path.join(WP_DIR, "default.png"))

    # Build the pack binary
    count = len(blobs)
    header_size = 5 + 2 + 2 + 8 + 4*count    # magic + ver + count + pad + offs[]
    out = bytearray()
    out += b"YWALL"
    out += bytes((2,))       # version 2
    out += bytes((count&0xFF,(count>>8)&0xFF))
    out += bytes(8)          # reserved
    # offsets placeholder (filled in below)
    offs_start = len(out)
    for _ in range(count): out += bytes(4)
    entry_offsets = []
    for i,b in enumerate(blobs):
        entry_offsets.append(len(out))
        out += bytes((W&0xFF,(W>>8)&0xFF,H&0xFF,(H>>8)&0xFF))
        out += bytes(4)      # reserved
        out += b
    # Patch offsets
    import struct
    for i,o in enumerate(entry_offsets):
        struct.pack_into("<I", out, offs_start + i*4, o)
    with open(OUT_BIN,"wb") as f: f.write(out)
    print(f"wrote {OUT_BIN}: {count} wallpapers, {len(out)} bytes")

if __name__ == "__main__":
    main()
