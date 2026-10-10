#!/usr/bin/env python3
import socket, time, subprocess, os, json
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.makedirs(f"{ROOT}/runlogs", exist_ok=True)

subprocess.run(["pkill", "-9", "qemu-system"], capture_output=True)
time.sleep(0.3)

for p in ["/tmp/q.sock", "/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass

disk_img = f"{ROOT}/yart-disk.img"
if not os.path.exists(disk_img):
    with open(disk_img, "wb") as f:
        f.truncate(64 * 1024 * 1024)

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

print("[*] Waiting for system to boot...")
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

# 1. Login screen
shot("step1_login")

# 2. Login
print("[*] Logging in...")
tap("ret")
time.sleep(0.4)
type_str("yart\n")
time.sleep(1.5)
shot("step2_desktop")

# 3. Open Nyra terminal using Super+T
print("[*] Opening Nyra terminal via Super+T...")
ev("meta_l", True)
time.sleep(0.1)
tap("t")
time.sleep(0.1)
ev("meta_l", False)
time.sleep(1.5)
shot("step3_terminal_opened")

# 4. In Nyra terminal, run apk list
print("[*] Running apk list...")
type_str("apk list\n")
time.sleep(1.5)
shot("step4_apk_list")

# 5. In Nyra terminal, run apk add discord (remote package network query test)
print("[*] Running apk add discord...")
type_str("apk add discord\n")
time.sleep(3.5)
shot("step5_apk_discord")

# 6. Run /bin/calc
print("[*] Running /bin/calc...")
type_str("/bin/calc\n")
time.sleep(2.0)
shot("step6_calc_window")

# 7. Mouse drag title bar:
# Relative mouse movements to title bar and drag
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

print("[*] Testing mouse title bar drag...")
send_mouse(0, 0, True)
time.sleep(0.1)
for _ in range(12):
    send_mouse(15, 10, True)
time.sleep(0.1)
send_mouse(0, 0, False)
time.sleep(0.5)
shot("step7_dragged_window")

# 8. Open Launcher / App Grid using Super
print("[*] Opening App Launcher grid...")
tap("f1")
time.sleep(1.5)
shot("step8_app_grid")

h.close()
s.close()
proc.terminate()
print("[*] Verification suite complete!")
