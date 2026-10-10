#!/usr/bin/env python3
import socket, time, subprocess, os, json, http.server, threading
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
time.sleep(0.3)
try:
    os.unlink(f"{ROOT}/runlogs/serial.log")
except OSError:
    pass

# Create a sample discord.ypkg package for the mock repo server
repo_dir = f"{ROOT}/_mock_repo"
os.makedirs(f"{repo_dir}/repo", exist_ok=True)
subprocess.run([
    "python3", f"{ROOT}/scripts/mkpkg.py", f"{repo_dir}/repo/discord.ypkg",
    "--name", "discord", "--version", "0.0.1",
    "--desc", "Discord Client (Mock Package)", "--icon", "terminal",
    "--file", f"{ROOT}/build/sysinfo.elf:/bin/discord:exec"
], capture_output=True)

# Start a background HTTP server on port 8000 serving repo_dir
class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=repo_dir, **kwargs)
    def log_message(self, format, *args):
        print(f"[HTTP Server] {self.address_string()} - {format % args}")

httpd = http.server.HTTPServer(("127.0.0.1", 8000), QuietHandler)
srv_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
srv_thread.start()
print("[*] Mock HTTP package repo running on 127.0.0.1:8000")

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

# Read initial QMP greeting banner
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

def ev(name, down):
    qmp_cmd("input-send-event", {
        "events": [{"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": down}}]
    })
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
        sz = os.path.getsize(ppm_path)
        print(f"[debug] shot {name}: res={res}, ppm_size={sz}")
        img = Image.open(ppm_path)
        img.save(png_path)
        os.unlink(ppm_path)
        print(f"[*] Saved screenshot: {png_path}")
    else:
        print(f"[debug] shot {name}: NO PPM! res={res}")

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
time.sleep(2.0)

# Test apk list
print("[*] Running apk list...")
type_str("apk list\n")
time.sleep(2.5)
shot("shot_1_apk_list")

# Verify the unprivileged package manager does not elevate or contact the
# development-only repository.
print("[*] Checking apk add discord privilege gate...")
type_str("apk add discord\n")
time.sleep(1.0)
shot("shot_2_apk_discord_blocked")

# Run the installed package /bin/discord
print("[*] Running installed /bin/discord...")
type_str("/bin/discord\n")
time.sleep(3.0)
shot("shot_3_discord_executed")

s.close()
proc.terminate()
httpd.shutdown()
print("[*] APK network test completed!")
