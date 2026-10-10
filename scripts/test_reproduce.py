#!/usr/bin/env python3
import socket, time, subprocess, os, json
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
subprocess.run(["pkill", "-9", "qemu-system"], capture_output=True)
time.sleep(0.3)
try:
    os.unlink(f"{ROOT}/runlogs/serial.log")
except OSError:
    pass

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

# Run apk add discord
print("[*] Running apk add discord (with NO mock server)...")
type_str("apk add discord\n")
time.sleep(10.0)

shot("reproduce_after_apk")

s.close()
proc.terminate()
print("[*] Reproduction test finished!")
