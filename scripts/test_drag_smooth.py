#!/usr/bin/env python3
import socket, time, subprocess, os, json
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
time.sleep(0.3)

for p in ["/tmp/q.sock", "/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass

disk_img = f"{ROOT}/yart-disk.img"
cmd = [
    "qemu-system-x86_64", "-cpu", "max", "-smp", "4", "-m", "1024", "-machine", "q35",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-cdrom", f"{ROOT}/yart.iso",
    "-drive", f"file={disk_img},format=raw,if=virtio",
    "-display", "none", "-vga", "std", "-rtc", "base=localtime",
    "-serial", f"file:{ROOT}/runlogs/serial.log",
    "-qmp", "unix:/tmp/q.sock,server=on,wait=off",
    "-monitor", "unix:/tmp/qm.sock,server=on,wait=off",
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

def r():
    b = b""
    while not b.endswith(b"\n"):
        b += s.recv(4096)
    return b

r()
s.sendall(b'{"execute":"qmp_capabilities"}\n')
r()
s.sendall(b'{"execute":"cont"}\n')
r()

def ev(name, down):
    s.sendall(json.dumps({
        "execute": "input-send-event",
        "arguments": {"events": [{"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": down}}]}
    }).encode() + b"\n")
    r()
    time.sleep(0.04)

def tap(name):
    ev(name, True)
    time.sleep(0.05)
    ev(name, False)
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
            ev("shift", True)
            tap("minus")
            ev("shift", False)
        elif ch == '\n':
            tap("ret")
        else:
            tap(ch)

print("[*] Booting...")
time.sleep(12)

h = socket.socket(socket.AF_UNIX)
h.connect("/tmp/qm.sock")
h.settimeout(5)

def shot(name):
    ppm_path = f"{ROOT}/{name}.ppm"
    png_path = f"{ROOT}/{name}.png"
    h.sendall(f"screendump {ppm_path}\n".encode())
    time.sleep(0.5)
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
ev("meta_l", True)
time.sleep(0.08)
tap("t")
time.sleep(0.08)
ev("meta_l", False)
time.sleep(1.5)

# Run calc
type_str("/bin/calc\n")
time.sleep(2.0)

def send_mouse(dx, dy, btn_left=False):
    events = [
        {"type": "rel", "data": {"axis": "x", "value": dx}},
        {"type": "rel", "data": {"axis": "y", "value": dy}},
        {"type": "btn", "data": {"button": "left", "down": btn_left}}
    ]
    s.sendall(json.dumps({
        "execute": "input-send-event",
        "arguments": {"events": events}
    }).encode() + b"\n")
    r()
    time.sleep(0.04)

# Move mouse to top-left corner (0,0)
for _ in range(5):
    send_mouse(-300, -300, False)
time.sleep(0.1)

# Move to titlebar (560, 145)
send_mouse(560, 145, False)
time.sleep(0.2)
shot("drag_smooth_1_init")

# Drag by -180px horizontally and +100px vertically (landing at x=380, y=245, well away from any screen edge)
send_mouse(0, 0, True)
time.sleep(0.1)

for _ in range(9):
    send_mouse(-20, 11, True)
    time.sleep(0.04)

shot("drag_smooth_2_during")

send_mouse(0, 0, False)
time.sleep(0.4)
shot("drag_smooth_3_final")

h.close()
s.close()
proc.terminate()
print("[*] Finished!")
