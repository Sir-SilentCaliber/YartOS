#!/usr/bin/env python3
"""Boot YartOS, drive the cursor via HMP mouse, and screenshot each UI state:
desktop, quick settings, clipboard popover, network list, launcher.
Also runs programmatic pixel checks."""
import socket, subprocess, time, os, sys
from PIL import Image

# Portable: resolve the repo root from this script's location, not a
# hardcoded absolute path, so the project can be cloned anywhere.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISK = os.path.join(ROOT, "yart-disk.img")
if not os.path.exists(DISK):
    open(DISK, "wb").write(b"\0" * (64 << 20))

subprocess.run(["pkill", "-9", "qemu-system"], capture_output=True); time.sleep(0.4)
for p in ["/tmp/q.sock", "/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass
SERIAL = ROOT + "/runlogs/serial.log"
os.makedirs(os.path.dirname(SERIAL), exist_ok=True)
if os.path.exists(SERIAL): os.unlink(SERIAL)

proc = subprocess.Popen([
    "qemu-system-x86_64", "-cpu", "max", "-smp", "4", "-m", "1024", "-machine", "q35",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-acpitable", "file=" + ROOT + "/acpi/battery.aml",
    "-cdrom", ROOT + "/yart.iso",
    "-drive", "file=" + DISK + ",format=raw,if=none,id=vda", "-device", "virtio-blk-pci,drive=vda",
    "-boot", "d", "-display", "none", "-vga", "std",
    "-serial", "file:" + SERIAL,
    "-qmp", "unix:/tmp/q.sock,server=on,wait=off",
    "-monitor", "unix:/tmp/qm.sock,server=on,wait=off",
    "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
    "-rtc", "base=localtime", "-S"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# QMP: capabilities + cont on ONE session
for _ in range(60):
    try:
        q = socket.socket(socket.AF_UNIX); q.connect("/tmp/q.sock"); break
    except OSError: time.sleep(0.2)
q.settimeout(5)
def qr():
    b = b""
    while not b.endswith(b"\n"): b += q.recv(4096)
    return b
qr(); q.sendall(b'{"execute":"qmp_capabilities"}\n'); qr()
q.sendall(b'{"execute":"cont"}\n'); qr()

# HMP monitor
for _ in range(60):
    try:
        h = socket.socket(socket.AF_UNIX); h.connect("/tmp/qm.sock"); break
    except OSError: time.sleep(0.2)
h.settimeout(5); time.sleep(0.2)
def hmp(cmd):
    h.sendall((cmd + "\n").encode()); time.sleep(0.25)
    try: h.recv(65536)
    except socket.timeout: pass

def shot(name):
    hmp("screendump %s/%s.ppm" % (ROOT, name))
    im = Image.open("%s/%s.ppm" % (ROOT, name)).convert("RGB")
    im.save("%s/%s.png" % (ROOT, name))
    return im

# cursor state (matches WM init: center)
cx, cy = 640, 400
def move_to(tx, ty, steps=40):
    global cx, cy
    for _ in range(steps):
        dx = (tx - cx) // steps; dy = (ty - cy) // steps
        cx += dx; cy += dy
        hmp("mouse_move %d %d" % (dx, dy))
    cx, cy = tx, ty
    time.sleep(0.15)

def click():
    hmp("mouse_button 1"); time.sleep(0.06); hmp("mouse_button 0"); time.sleep(0.4)

print("[*] waiting for boot ...")
time.sleep(20)

im = shot("ui-desktop")
print("[*] desktop shot saved")

# 1. click status cluster (top-right) -> quick settings
move_to(1210, 20); click()
shot("ui-quick")
print("[*] quick settings shot saved")

# 2. click wifi chevron (rightmost button ~ x=1190)
move_to(1195, 20); click()
shot("ui-netlist")
print("[*] network list shot saved")

# 3. click clipboard button (~ x=1155)
move_to(1158, 20); click()
shot("ui-clipboard")
print("[*] clipboard shot saved")

# 4. click language button (~ x=1120)
move_to(1120, 20); click()
shot("ui-lang")
print("[*] language toggled shot saved")

# 5. Super -> launcher
q.sendall(b'{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"meta_l"},"down":true}}]}}\n'); qr()
time.sleep(0.1)
q.sendall(b'{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"meta_l"},"down":false}}]}}\n'); qr()
time.sleep(0.5)
shot("ui-launcher")
print("[*] launcher shot saved")

# Esc close
q.sendall(b'{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"esc"},"down":true}}]}}\n'); qr()
time.sleep(0.05)
q.sendall(b'{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"esc"},"down":false}}]}}\n'); qr()

# pixel checks on desktop shot
im = Image.open(ROOT + "/ui-desktop.ppm").convert("RGB")
w, hh = im.size
px = im.load()
# panel area (y 0..32)
def region_mean(x0, y0, x1, y1):
    t = n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = px[x, y]; t += (r+g+b)//3; n += 1
    return t // max(n, 1)
print("[*] panel mean:", region_mean(0, 0, w, 32))
print("[*] dock mean:", region_mean(w//2-200, hh-80, w//2+200, hh-10))
print("[*] wallpaper mean:", region_mean(w//2, 300, w//2+100, 400))
print("done")
