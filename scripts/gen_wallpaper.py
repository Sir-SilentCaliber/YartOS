#!/usr/bin/env python3
"""Generate a calm, enterprise-grade dune wallpaper for Yart OS.

Source PNG lives in  kora/wallpapers/default.png
Packed binary goes to build/wallpaper.bin (linked into compositor at build time)
"""
import sys, os
try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.stderr.write("\nERROR: Pillow not found. Install with:\n  pip3 install Pillow\n  or: sudo apt install python3-pil\n\n")
    sys.exit(1)
import math, random

random.seed(42)
W,H = 1280,800
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_PNG = os.path.join(ROOT, "kora", "wallpapers", "default.png")
OUT_BIN = os.path.join(ROOT, "build", "wallpaper.bin")
os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)

def ridge(y_base, wavelist, step=2):
    pts=[]
    for x in range(0, W+step, step):
        y = float(y_base)
        for wlen, amp, phase in wavelist:
            y += amp*math.sin(x*(2*math.pi/wlen) + phase)
        pts.append((x, int(y)))
    return pts

img = Image.new("RGB",(W,H))
px = img.load()
for y in range(H):
    t = y/H
    r = int(0x22 + (0xCB-0x22)*t*0.92)
    g = int(0x29 + (0xC6-0x29)*t*0.88)
    b = int(0x38 + (0xBC-0x38)*t*0.82)
    for x in range(W): px[x,y] = (r,g,b)

glow = Image.new("RGBA",(W,H),(0,0,0,0))
d = ImageDraw.Draw(glow)
cx,cy = int(W*0.82), int(H*0.56)
for radius,alpha,col in [
    (600,14,(255,180,120)),(380,26,(255,198,145)),
    (210,44,(255,212,165)),(90,62,(255,225,185)),]:
    d.ellipse([cx-radius,cy-radius,cx+radius,cy+radius],fill=(*col,alpha))
glow = glow.filter(ImageFilter.GaussianBlur(100))
img = Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")

fill = Image.new("RGBA",(W,H),(0,0,0,0))
fd = ImageDraw.Draw(fill)
fcx,fcy = int(W*0.18), int(H*0.28)
fd.ellipse([fcx-320,fcy-280,fcx+320,fcy+280],fill=(120,160,210,16))
fill = fill.filter(ImageFilter.GaussianBlur(110))
img = Image.alpha_composite(img.convert("RGBA"), fill).convert("RGB")

haze = Image.new("RGBA",(W,H),(0,0,0,0))
hd = ImageDraw.Draw(haze)
for yy in range(int(H*0.44), int(H*0.62)):
    t = (yy - H*0.44)/(H*0.18)
    a = int(22*(1 - abs(2*t-1)))
    hd.line([(0,yy),(W,yy)], fill=(225,225,230,a))
haze = haze.filter(ImageFilter.GaussianBlur(28))
img = Image.alpha_composite(img.convert("RGBA"), haze).convert("RGB")

layers = [
    (int(H*0.50), [(1400,28,0.6),(600,10,2.1)],                          (148,152,162), 7.0),
    (int(H*0.58), [(1200,46,1.3),(520,12,0.3)],                          (112,118,130), 5.0),
    (int(H*0.66), [(1000,60,2.5),(440,12,1.0)],                          (80, 86, 98),  3.0),
    (int(H*0.75), [(850, 55,0.2),(360,12,3.3)],                          (54, 58, 70),  2.0),
    (int(H*0.87), [(1600,44,2.0),(520,6,1.0)],                           (32, 35, 44),  1.2),
]
for (y_base, waves, color, blur_r) in layers:
    layer = Image.new("RGB",(W,H), color)
    mask = Image.new("L",(W,H),0)
    md = ImageDraw.Draw(mask)
    pts = ridge(y_base, waves, step=2)
    poly = [(0,H)] + pts + [(W,H)]
    md.polygon(poly, fill=255)
    if blur_r > 0: mask = mask.filter(ImageFilter.GaussianBlur(blur_r))
    img.paste(layer, (0,0), mask)

vign = Image.new("RGBA",(W,H),(0,0,0,0))
vd = ImageDraw.Draw(vign)
cx2,cy2 = W//2, int(H*0.56)
maxr = math.hypot(W,H)*0.62
for y in range(0,H,8):
    for x in range(0,W,8):
        d = math.hypot(x-cx2,y-cy2)/maxr
        if d > 0.64:
            a = int(min(36, (d-0.64)*100))
            vd.rectangle([x,y,x+7,y+7],fill=(0,0,0,a))
vign = vign.filter(ImageFilter.GaussianBlur(50))
img = Image.alpha_composite(img.convert("RGBA"), vign).convert("RGB")

img.save(OUT_PNG,"PNG")

raw = bytearray()
for y in range(H):
    for x in range(W):
        r,g,b = img.getpixel((x,y))[0:3]
        raw += bytes((b,g,r,255))
with open(OUT_BIN,"wb") as f:
    f.write(b"YWALL")
    f.write(bytes((W&0xFF,(W>>8)&0xFF,H&0xFF,(H>>8)&0xFF)))
    f.write(bytes(7))
    f.write(raw)
print(f"wrote {OUT_PNG} {img.size}")
print(f"wrote {OUT_BIN} {len(raw)} bytes (BGRA)")
