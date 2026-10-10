#!/usr/bin/env python3
import socket, time, subprocess, os, json, http.server, threading
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
time.sleep(0.3)
try:
    os.unlink(f"{ROOT}/runlogs/serial.log")
except OSError:
    pass

for p in ["/tmp/q.sock", "/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass

disk_img = f"{ROOT}/yart-disk.img"
cmd = [
    "qemu-system-x86_64", "-cpu", "max", "-smp", "4", "-m", "1024", "-machine", "q35",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-cdrom", f"{ROOT}/yart.iso",
    "-drive", f"file={disk_img},format=raw,if=virtio",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0",
    "-display", "none", "-vga", "std", "-rtc", "base=localtime",
    "-serial", f"file:{ROOT}/runlogs/serial.log",
    "-qmp", "unix:/tmp/q.sock,server=on,wait=off",
    "-monitor", "unix:/tmp/qm.sock,server=on,wait=off",
    "-no-reboot", "-d", "int,cpu_reset", "-D", f"{ROOT}/qemu.log",
    "-S"
]

proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect("/tmp/q.sock")
        break
    except OSError:
        time.sleep(0.1)

s.settimeout(5)
sfile = s.makefile("r", encoding="utf-8")

for line in sfile:
    if "QMP" in line:
        break

def qmp_cmd(execute, args=None):
    req = {"execute": execute}
    if args:
        req["arguments"] = args
    s.sendall(json.dumps(req).encode() + b"\n")
    for line in sfile:
        line = line.strip()
        if not line: continue
        try:
            data = json.loads(line)
            if "return" in data or "error" in data:
                return data
        except Exception:
            pass

qmp_cmd("qmp_capabilities")
qmp_cmd("cont")

def key_ev(name, down):
    qmp_cmd("input-send-event", {
        "events": [{"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": down}}]
    })
    time.sleep(0.04)

def tap(name):
    key_ev(name, True)
    time.sleep(0.05)
    key_ev(name, False)
    time.sleep(0.08)

def type_str(st):
    for ch in st:
        if ch == ' ':
            tap("spc")
        elif ch == '-':
            tap("minus")
        elif ch == '/':
            tap("slash")
        elif ch == '.':
            tap("dot")
        elif ch == '_':
            key_ev("shift", True)
            tap("minus")
            key_ev("shift", False)
        elif ch == '\n':
            tap("ret")
        else:
            tap(ch)

def mouse_move(dx, dy):
    qmp_cmd("input-send-event", {
        "events": [
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}
        ]
    })
    time.sleep(0.03)

def mouse_button(button, down):
    qmp_cmd("input-send-event", {
        "events": [
            {"type": "btn", "data": {"button": button, "down": down}}
        ]
    })
    time.sleep(0.04)

print("[*] Waiting for boot...")
for _ in range(60):
    if os.path.exists(f"{ROOT}/runlogs/serial.log"):
        with open(f"{ROOT}/runlogs/serial.log", "r") as f:
            if "wallpaper loaded successfully" in f.read():
                break
    time.sleep(0.5)
time.sleep(3.0)

def shot(name):
    ppm_path = f"{ROOT}/{name}.ppm"
    png_path = f"{ROOT}/{name}.png"
    res = qmp_cmd("screendump", {"filename": ppm_path})
    time.sleep(0.3)
    if os.path.exists(ppm_path):
        img = Image.open(ppm_path)
        img.save(png_path)
        os.unlink(ppm_path)
        print(f"[*] Saved screenshot: {png_path}")

# Unlock
tap("ret")
time.sleep(0.4)
type_str("yart\n")
time.sleep(1.5)

# Open terminal
key_ev("meta_l", True)
time.sleep(0.08)
tap("t")
time.sleep(0.08)
key_ev("meta_l", False)
time.sleep(2.0)

# Move cursor from center (640, 400) to title bar (500, 180):
# dy = -220, dx = -140 using small steps (dx=-7, dy=-11) x 20
print("[*] Moving cursor to title bar...")
for _ in range(20):
    mouse_move(-7, -11)
    time.sleep(0.02)
time.sleep(0.3)

shot("test_drag_1_hover_title")

# Press left button on title bar
print("[*] Pressing left mouse button on title bar...")
mouse_button("left", True)
time.sleep(0.2)

# Drag by (+200, +100) -> (dx=+10, dy=+5) x 20
print("[*] Dragging window...")
for _ in range(20):
    mouse_move(10, 5)
    time.sleep(0.03)
time.sleep(0.2)

# Release left button
mouse_button("left", False)
time.sleep(0.5)

shot("test_drag_2_after_drag")

s.close()
proc.terminate()
print("[*] Drag test finished!")
