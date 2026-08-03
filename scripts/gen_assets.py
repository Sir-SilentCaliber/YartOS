#!/usr/bin/env python3
"""Render Kora KDE icon SVGs into a flat RGBA binary (build/kora.bin) and a C
header (build/kora.h) with the icon enum.

Source Kora SVGs live in  kora/icons/kora/{apps,actions,places,...}/
Outputs go to            build/kora.bin  +  build/kora.h
"""
import io, os, struct, subprocess, sys

# --- Dependency check ---
try:
    subprocess.check_output(["rsvg-convert", "--version"], stderr=subprocess.STDOUT)
except (FileNotFoundError, subprocess.CalledProcessError):
    sys.stderr.write(
        "\nERROR: rsvg-convert not found.\n"
        "Install librsvg2-bin first:\n"
        "  Debian/Ubuntu:  sudo apt install librsvg2-bin python3-pil\n"
        "  Arch:           sudo pacman -S librsvg python-pillow\n"
        "  Fedora:         sudo dnf install librsvg2-tools python3-pillow\n"
        "  macOS:          brew install librsvg python3 && pip3 install pillow\n\n"
    )
    sys.exit(1)
try:
    from PIL import Image
except ImportError:
    sys.stderr.write(
        "\nERROR: Pillow (Python imaging) not found.\n"
        "  pip3 install Pillow\n"
        "  or install python3-pil via your package manager.\n\n"
    )
    sys.exit(1)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
KORA = os.path.join(ROOT, "kora", "icons", "kora")
OUT_H  = os.path.join(ROOT, "build", "kora.h")
OUT_BIN = os.path.join(ROOT, "build", "kora.bin")
os.makedirs(os.path.dirname(OUT_H), exist_ok=True)

# Preferred sizes
SZ_TRAY   = 22
SZ_DOCK   = 48
SZ_WIN    = 22
SZ_APP    = 48
SZ_CURSOR = 24

# Aliases: Kora ships some standard fdo icon names under different filenames.
# Map our requested name -> (actual_filename, category_override_or_None)
ALIASES = {
    # Cursors: Kora is an icon theme, not a cursor theme. Draw all cursor sprites
    # procedurally in a clean macOS/Adwaita-hybrid style so they look crisp at 24px.
    "cursor-arrow":              ("__proc_arrow",            "__builtin__"),
    "tool-pointer":              ("__proc_arrow",            "__builtin__"),
    "default":                   ("__proc_arrow",            "__builtin__"),
    "pointer":                   ("__proc_hand",             "__builtin__"),     # hand/link
    "hand":                      ("__proc_hand",             "__builtin__"),
    "hand1":                     ("__proc_hand",             "__builtin__"),
    "hand2":                     ("__proc_hand",             "__builtin__"),
    "text":                      ("__proc_ibeam",            "__builtin__"),     # text select
    "xterm":                     ("__proc_ibeam",            "__builtin__"),
    "wait":                      ("__proc_wait",             "__builtin__"),     # busy/hourglass
    "watch":                     ("__proc_wait",             "__builtin__"),
    "progress":                  ("__proc_wait",             "__builtin__"),     # arrow+watch
    "not-allowed":               ("__proc_forbidden",        "__builtin__"),     # circle-slash
    "forbidden":                 ("__proc_forbidden",        "__builtin__"),
    "no-drop":                   ("__proc_forbidden",        "__builtin__"),
    "col-resize":                ("__proc_hresize",          "__builtin__"),
    "row-resize":                ("__proc_vresize",          "__builtin__"),
    "ew-resize":                 ("__proc_hresize",          "__builtin__"),
    "ns-resize":                 ("__proc_vresize",          "__builtin__"),
    # Dialog buttons Kora doesn't ship by the standard names -- draw procedurally
    "dialog-ok":                 ("__proc_checkmark",        "__builtin__"),
    "dialog-apply":              ("__proc_checkmark",        "__builtin__"),
    "dialog-cancel":             ("__proc_xmark",            "__builtin__"),
    "dialog-close":              ("window-close",            None),
    "dialog-information":        ("dialog-information",      "status"),
    # Network tray
    "network-wireless-connected-100": ("network-wireless-signal-excellent", None),
    "network-offline":           ("network-disconnect",      None),
    # Mic tray
    "audio-input-microphone-high": ("microphone-sensitivity-high", None),
    # Actions
    "system-search":             ("search",                  "apps"),
    # Devices
    "display":                   ("video-display",           None),
    "phone":                     ("smartphone",              None),
    # Mimetypes where Kora only ships -symbolic or uses different names
    "application-x-compressed":  ("application-archive",     None),
    "package-x-generic":         ("application-archive",     None),
    "text-x-generic":            ("text-x-generic",          None),
    "application-x-executable":  ("application-x-executable",None),
    "video-x-generic":           ("video-x-generic",         None),
    "audio-x-generic":           ("audio-x-generic",         None),
    "x-office-calendar":         ("x-office-calendar",       None),
    "x-office-spreadsheet":      ("x-office-spreadsheet",    None),
    "x-office-document":         ("x-office-document",       None),
    "x-office-presentation":     ("x-office-presentation",   None),
    # Settings
    "preferences-desktop":       ("preferences-desktop",     None),
}

# Procedurally-drawn builtin icons (RGBA tuples) keyed by name
def _builtin_checkmark(size):
    """Green checkmark on transparent bg."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    pad = max(2, size//8)
    # Check path: \/ shape
    pts = [(pad, size//2), (size//3, size-pad-1), (size-pad, pad+1)]
    d.line(pts, fill=(80,200,120,255), width=max(2,size//8))
    return im.tobytes()

def _builtin_xmark(size):
    """Red X on transparent bg."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    pad = max(2, size//4)
    w = max(2, size//8)
    d.line([(pad,pad),(size-pad,size-pad)], fill=(230,80,80,255), width=w)
    d.line([(size-pad,pad),(pad,size-pad)], fill=(230,80,80,255), width=w)
    return im.tobytes()

# --------------- Cursors (24px canvas; drawn with soft drop-shadow + 1px outline)
def _shadow(d, pts, fill):
    """Draw a soft 2px drop shadow by offsetting +1,+1,+2,+2 at low alpha."""
    from PIL import ImageDraw as _D
    for ox,oy,a in [(1,1,30),(2,2,20),(2,1,16),(1,2,16)]:
        sh = [(x+ox, y+oy) for x,y in pts]
        d.polygon(sh, fill=(0,0,0,a))

def _curs_outline(d, pts, width, outline):
    d.line(pts + [pts[0]], fill=outline, width=width, joint="curve")

def _builtin_arrow(size):
    """White arrow pointer with soft shadow + dark outline (Adwaita/mac hybrid).
    Hotspot = tip at (2,2) in 24px coords."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size / 24.0
    def pt(x,y): return (x*k, y*k)
    # Classic Windows/mac arrow shape: tip top-left, stem down then right, flat tail.
    # Points chosen to be pixel-snappy at 24px and look good at 5x upscale.
    poly = [pt(2,2), pt(2,17), pt(6,13), pt(9,18), pt(11,17), pt(8,12), pt(14,12)]
    _shadow(d, poly, (0,0,0))
    d.polygon(poly, fill=(248,249,251,255))
    _curs_outline(d, poly, max(1,int(round(1.2*k))), (24,28,36,230))
    # Top-left inner highlight
    hl = [pt(3,3), pt(3,15), pt(6,12)]
    d.line(hl, fill=(255,255,255,55), width=max(1,int(k)), joint="curve")
    return im.tobytes()

def _builtin_hand(size):
    """Pointing hand (link/hover). Hotspot at index fingertip ~(12,3)."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    def pt(x,y): return (x*k, y*k)
    # Standard pointing-hand (link) cursor: index finger up, others curled, thumb in.
    poly = [pt(11,3), pt(13,3), pt(13,10), pt(15,10), pt(15,7), pt(17,7),
            pt(17,11), pt(19,11), pt(19,8), pt(21,8), pt(21,15),
            pt(19,19), pt(15,21), pt(9,21), pt(5,17), pt(5,12),
            pt(8,12), pt(8,15), pt(10,15), pt(10,9)]
    _shadow(d, poly, (0,0,0))
    d.polygon(poly, fill=(248,249,251,255))
    _curs_outline(d, poly, max(1,int(round(1.2*k))), (24,28,36,230))
    return im.tobytes()

def _builtin_ibeam(size):
    """Text I-beam. Hotspot at center (~12,12)."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    w = max(1, int(round(1.5*k)))
    col = (244,245,247,255)
    out = (10,12,16,255)
    cx = 12*k
    # vertical stem
    d.line([(cx,4*k),(cx,20*k)], fill=out, width=w+2)
    d.line([(cx,4*k),(cx,20*k)], fill=col, width=w)
    # top and bottom serifs
    for yy in (4*k, 20*k):
        d.line([(cx-4*k, yy),(cx+4*k, yy)], fill=out, width=w+2)
        d.line([(cx-4*k, yy),(cx+4*k, yy)], fill=col, width=w)
    return im.tobytes()

def _builtin_wait(size):
    """Busy: spinning ring / watch. Drawn as a static ring with a gap so
    users see 'something is happening' without animation in the sprite."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    cx=cy=12*k; r=6*k
    # soft shadow circle
    d.ellipse([cx-r-1, cy-r-1, cx+r+1, cy+r+1], outline=(0,0,0,40), width=max(2,int(2*k)))
    d.arc([cx-r, cy-r, cx+r, cy+r], start=45, end=315,
          fill=(244,245,247,255), width=max(2,int(2*k)))
    # little arrow in the corner (progress = arrow+watch)
    tip=(2*k,2*k); re=(7*k,7*k); notch=(4*k,8*k); sb=(4*k,15*k); sr=(6*k,15*k)
    tail=(10*k,11*k); fr=(14*k,11*k)
    poly=[tip,re,notch,sb,sr,tail,fr]
    d.polygon(poly, fill=(244,245,247,255))
    _curs_outline(d, poly, max(1,int(round(1.3*k))), (10,12,16,255))
    return im.tobytes()

def _builtin_forbidden(size):
    """Circle with slash (no-drop / not-allowed). Hotspot in center."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    cx=cy=12*k; r=8*k
    d.ellipse([cx-r-1,cy-r-1,cx+r+1,cy+r+1], outline=(0,0,0,40), width=max(2,int(2*k)))
    d.ellipse([cx-r,cy-r,cx+r,cy+r], outline=(220,70,70,255), width=max(2,int(2*k)), fill=(0,0,0,0))
    # slash
    import math
    a = 3*math.pi/4
    x1=cx+r*0.7*math.cos(a); y1=cy+r*0.7*math.sin(a)
    x2=cx-r*0.7*math.cos(a); y2=cy-r*0.7*math.sin(a)
    d.line([(x1,y1),(x2,y2)], fill=(220,70,70,255), width=max(2,int(2*k)))
    return im.tobytes()

def _builtin_hresize(size):
    """Horizontal-resize: <-> arrow. Hotspot in center."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    cy=12*k
    d.line([(5*k,cy),(19*k,cy)], fill=(244,245,247,255), width=max(1,int(2*k)))
    col=(244,245,247,255); out=(10,12,16,255); w=max(1,int(1.5*k))
    for x,dx in [(5*k,1),(19*k,-1)]:
        pts=[(x,cy),(x+3*k*dx,cy-3*k),(x+3*k*dx,cy+3*k)]
        d.polygon(pts, fill=col)
        d.line(pts+[pts[0]], fill=out, width=w)
    return im.tobytes()

def _builtin_vresize(size):
    """Vertical-resize arrow."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0,0,0,0))
    d = ImageDraw.Draw(im)
    k = size/24.0
    cx=12*k
    d.line([(cx,5*k),(cx,19*k)], fill=(244,245,247,255), width=max(1,int(2*k)))
    col=(244,245,247,255); out=(10,12,16,255); w=max(1,int(1.5*k))
    for y,dy in [(5*k,1),(19*k,-1)]:
        pts=[(cx,y),(cx-3*k,y+3*k*dy),(cx+3*k,y+3*k*dy)]
        d.polygon(pts, fill=col)
        d.line(pts+[pts[0]], fill=out, width=w)
    return im.tobytes()

BUILTINS = {
    "__proc_checkmark": _builtin_checkmark,
    "__proc_xmark":     _builtin_xmark,
    "__proc_arrow":     _builtin_arrow,
    "__proc_hand":      _builtin_hand,
    "__proc_ibeam":     _builtin_ibeam,
    "__proc_wait":      _builtin_wait,
    "__proc_forbidden": _builtin_forbidden,
    "__proc_hresize":   _builtin_hresize,
    "__proc_vresize":   _builtin_vresize,
}

def find(name, category, size=None, symbolic=False):
    """Search Kora icon tree for an icon.

    Search order (per category):
      1. scalable/  -> <name>.svg                       (full-color preferred)
      2. scalable/  -> <name>-symbolic.svg
      3. symbolic/  -> <name>-symbolic.svg              (explicit symbolic folder)
      4. scalable@2/, <closest numeric>/, <n>@2/ (same suffix ordering)
    If `symbolic=True`, symbolic variants are preferred.
    Falls back across categories; tries ALIASES table if first pass fails.
    """
    cats = [category] if category else []
    cats += ["apps", "actions", "places", "devices", "status", "categories",
             "emblems", "mimetypes", "panel", "animations", "emotes"]

    def try_name(nm, cat):
        base = f"{KORA}/{cat}"
        if not os.path.isdir(base): return None
        try:
            entries = set(os.listdir(base))
        except OSError:
            return None
        # Build ordered list of subfolders to search
        folder_order = []
        for sz_name in ["scalable", "scalable@2"]:
            if sz_name in entries: folder_order.append(sz_name)
        nums = []
        for e in entries:
            try: nums.append(int(e))
            except ValueError: pass
        if size: nums.sort(key=lambda s: (abs(s-size), -s))
        else:    nums.sort(reverse=True)
        for n in nums:
            if str(n) in entries: folder_order.append(str(n))
            n2 = f"{n}@2"
            if n2 in entries: folder_order.append(n2)
        # Also include symbolic/ folder itself (Kora ships standalone symbolic/ dirs)
        if "symbolic" in entries: folder_order.append("symbolic")
        if "symbolic@2" in entries: folder_order.append("symbolic@2")

        # Suffix order depends on symbolic preference
        if symbolic:
            suffixes = (f"-symbolic.svg", ".svg")
        else:
            suffixes = (".svg", f"-symbolic.svg")

        for sz in folder_order:
            for suf in suffixes:
                # When inside a folder named 'symbolic' or 'symbolic@2', the file
                # inside is usually already named <name>-symbolic.svg, but sometimes
                # just <name>.svg -- try both.
                if sz.startswith("symbolic"):
                    cand = f"{base}/{sz}/{nm}-symbolic.svg"
                    if os.path.exists(cand): return cand
                    cand = f"{base}/{sz}/{nm}.svg"
                    if os.path.exists(cand): return cand
                else:
                    cand = f"{base}/{sz}/{nm}{suf}"
                    if os.path.exists(cand): return cand
        return None

    # Try the requested name first in category-preferred order
    for cat in cats:
        p = try_name(name, cat)
        if p: return ("file", p)
    # If not found, try alias
    if name in ALIASES:
        real, cat_over = ALIASES[name]
        if cat_over == "__builtin__":
            return ("builtin", real)
        al_cats = [cat_over] if cat_over else cats
        for cat in al_cats:
            p = try_name(real, cat)
            if p: return ("file", p)
    return None

def render_svg(svg, size, css=None):
    args = ["rsvg-convert", "-w", str(size), "-h", str(size), "-f", "png"]
    if css:
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".css", delete=False) as cf:
            cf.write(css); cf.flush()
            args += ["--stylesheet", cf.name]; args.append(svg)
            png = subprocess.check_output(args)
        os.unlink(cf.name)
    else:
        args.append(svg); png = subprocess.check_output(args)
    from PIL import Image
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    if im.size != (size,size):
        im = im.resize((size,size), Image.LANCZOS)
    return im.tobytes()

ICONS = []
def add(name, filename, cat, size=SZ_APP, sym=False):
    ICONS.append((name, filename, cat, size, sym))

# Tray (symbolic)
add("tray_net_wired",    "network-wired",                  "panel",  SZ_TRAY, sym=True)
add("tray_net_wifi",     "network-wireless-connected-100", "panel",  SZ_TRAY, sym=True)
add("tray_net_idle",     "network-offline",                "panel",  SZ_TRAY, sym=True)
add("tray_audio_hi",     "audio-volume-high",              "panel",  SZ_TRAY, sym=True)
add("tray_audio_mute",   "audio-volume-muted",             "panel",  SZ_TRAY, sym=True)
add("tray_battery",      "battery-090",                    "panel",  SZ_TRAY, sym=True)
add("tray_shutdown",     "system-shutdown",                "actions",SZ_TRAY, sym=True)
add("tray_lock",         "system-lock-screen",             "actions",SZ_TRAY, sym=True)
add("tray_user",         "avatar-default",                 "panel",  SZ_TRAY, sym=True)
add("tray_bluetooth",    "bluetooth-active",               "status", SZ_TRAY, sym=True)
add("tray_airplane",     "airplane-mode",                  "status", SZ_TRAY, sym=True)
add("tray_microphone",   "audio-input-microphone-high",    "panel",  SZ_TRAY, sym=True)

# Dock
add("dock_terminal",     "utilities-terminal",             "apps",   SZ_DOCK)
add("dock_files",        "system-file-manager",            "apps",   SZ_DOCK)
add("dock_browser",      "web-browser",                    "apps",   SZ_DOCK)
add("dock_editor",       "accessories-text-editor",        "apps",   SZ_DOCK)
add("dock_settings",     "preferences-system",             "categories", SZ_DOCK)
add("dock_launcher",    "applications-all",               "apps",   SZ_DOCK)
add("dock_trash",       "user-trash",                     "places", SZ_DOCK)

# Places
add("place_desktop",     "user-desktop",                   "places", SZ_APP)
add("place_docs",        "folder-documents",               "places", SZ_APP)
add("place_dl",          "folder-download",                "places", SZ_APP)
add("place_music",       "folder-music",                   "places", SZ_APP)
add("place_pics",        "folder-pictures",                "places", SZ_APP)
add("place_videos",      "folder-videos",                  "places", SZ_APP)
add("place_home",        "user-home",                      "places", SZ_APP)
add("place_folder",      "folder",                         "places", SZ_APP)
add("place_drive",       "drive-harddisk",                 "devices",SZ_APP)

# Window controls
add("win_close",         "window-close",                   "actions",SZ_WIN, sym=True)
add("win_max",           "window-maximize",                "actions",SZ_WIN, sym=True)
add("win_min",           "window-minimize",                "actions",SZ_WIN, sym=True)
add("win_restore",       "window-restore",                 "actions",SZ_WIN, sym=True)

# Actions
for a in ["go-up","go-down","go-next","go-previous","go-home","document-new",
         "document-open","document-save","document-save-as","edit-delete",
         "edit-cut","edit-copy","edit-paste","edit-undo","edit-redo",
         "edit-find","view-refresh","view-fullscreen","application-exit",
         "list-add","list-remove","zoom-in","zoom-out","system-run","system-search",
         "media-playback-start","media-playback-pause","media-playback-stop",
         "media-skip-forward","media-skip-backward","media-seek-forward","media-seek-backward",
         "audio-volume-high","audio-volume-low",
         "dialog-ok","dialog-cancel","dialog-close","dialog-apply","dialog-warning","dialog-error","dialog-information",
         "folder-new","go-jump","preferences-desktop","help-about"]:
    add("act_" + a.replace("-", "_"), a, "actions", SZ_WIN, sym=True)

# Devices
for d in ["drive-harddisk","drive-removable-media","drive-optical",
         "audio-card","input-keyboard","input-mouse","input-tablet","camera-photo",
         "camera-video","display","printer","scanner","computer","phone","network-wired","network-wireless"]:
    add("dev_" + d.replace("-", "_"), d, "devices", 32)

# Mimetypes
for m in ["text-x-generic","text-html","text-css","text-x-script","application-x-executable",
         "folder","package-x-generic","image-x-generic","video-x-generic","audio-x-generic",
         "application-pdf","x-office-calendar","x-office-spreadsheet","x-office-document","x-office-presentation",
         "application-x-compressed"]:
    add("mime_" + m.replace("-","_").replace("+","_").replace(".","_"), m, "mimetypes", 32)

# Cursors (all 24px, procedurally drawn)
add("cursor_arrow",      "default",                        "actions",SZ_CURSOR)
add("cursor_hand",       "pointer",                        "actions",SZ_CURSOR)
add("cursor_ibeam",      "text",                           "actions",SZ_CURSOR)
add("cursor_wait",       "wait",                           "actions",SZ_CURSOR)
add("cursor_forbidden",  "forbidden",                      "actions",SZ_CURSOR)
add("cursor_hresize",    "col-resize",                     "actions",SZ_CURSOR)
add("cursor_vresize",    "row-resize",                     "actions",SZ_CURSOR)
add("cursor_prefs",      "preferences-desktop-cursors",    "apps",   32)

def main():
    bin_data = bytearray()
    entries = []
    bin_data += b"YICON" + struct.pack("<BH", 2, len(ICONS))
    bin_data += b"\x00" * (16 - len(bin_data))
    assert len(bin_data) == 16
    table_off = len(bin_data)
    bin_data += b"\x00" * (16 * len(ICONS))

    name_buf = bytearray()
    missing = 0
    for i,(name, fname, cat, size, sym) in enumerate(ICONS):
        found = find(fname, cat, size=size, symbolic=sym)
        if found is None:
            print(f"MISSING icon: {name} ({fname})", file=sys.stderr)
            missing += 1
            raw = b"\x00\x00\x00\x00"; w = h = 1
        else:
            kind, path = found
            try:
                if kind == "builtin":
                    raw = BUILTINS[path](size)
                else:
                    raw = render_svg(path, size)
            except Exception as e:
                print(f"FAIL icon {name} ({path}): {e}", file=sys.stderr)
                missing += 1
                raw = b"\x00\x00\x00\x00"; w=h=1; continue
            w = h = size
            assert len(raw) == w*h*4, f"{name}: bad pixel len {len(raw)}"
        n_off = len(name_buf)
        name_buf += name.encode() + b"\x00"
        px_off = len(bin_data)
        bin_data += raw
        entries.append((name, n_off, px_off, w, h))
    name_off_in_bin = len(bin_data)
    bin_data += name_buf
    for i, (name, n_off, px_off, w, h) in enumerate(entries):
        ent = table_off + i*16
        struct.pack_into("<IHIHHH", bin_data, ent,
                         name_off_in_bin + n_off,
                         len(name),
                         px_off, w, h, w*4)
    with open(OUT_BIN, "wb") as f: f.write(bin_data)
    with open(OUT_H, "w") as f:
        f.write("/* Auto-generated by scripts/gen_assets.py */\n")
        f.write("#pragma once\n#include \"sys.h\"\n")
        f.write(f"#define ASSET_BIN_SIZE {len(bin_data)}\n")
        f.write(f"#define ASSET_ICON_COUNT {len(ICONS)}\n")
        f.write("enum {\n")
        for i,(name,*_) in enumerate(ICONS):
            f.write(f"    ICON_{name.upper()} = {i},\n")
        f.write(f"    ICON_COUNT = {len(ICONS)}\n")
        f.write("};\n")
    print(f"wrote {OUT_BIN} ({len(bin_data)} bytes, {len(ICONS)} icons, {missing} missing)")
    print(f"wrote {OUT_H}")

if __name__ == "__main__":
    main()
