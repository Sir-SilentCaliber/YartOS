#!/usr/bin/env python3
"""Boot YartOS in QEMU (q35+OVMF, with the ACPI battery SSDT injected),
wait for the desktop, and capture a screenshot + serial log tail.

Usage: python3 scripts/boot_shot.py [wait_seconds]
"""
import socket, subprocess, sys, time, os, glob

# Portable: resolve the repo root from this script's location.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "yart.iso")
OVMF = None
for _c in ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd",
           "/usr/share/ovmf/OVMF.fd", "/usr/share/edk2/x64/OVMF_CODE.fd"):
    if os.path.exists(_c):
        OVMF = _c
        break
DISK = os.path.join(ROOT, "yart-disk.img")

# Create the 64 MiB persistent disk on first run (it is a local artifact,
# not part of the portable tree).
if not os.path.exists(DISK):
    with open(DISK, "wb") as _f:
        _f.truncate(64 * 1024 * 1024)
BATAML = os.path.join(ROOT, "acpi", "battery.aml")
SERIAL = os.path.join(ROOT, "runlogs", "serial.log")
SHOT = os.path.join(ROOT, "shot.ppm")
PNG = os.path.join(ROOT, "shot.png")

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 22

subprocess.run(["pkill", "-9", "qemu-system"], capture_output=True)
time.sleep(0.4)
for p in ["/tmp/q.sock", "/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass
os.makedirs(os.path.dirname(SERIAL), exist_ok=True)
if os.path.exists(SERIAL):
    os.unlink(SERIAL)

cmd = [
    "qemu-system-x86_64",
    "-cpu", "max", "-smp", "4", "-m", "1024",
    "-machine", "q35",
    "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF}",
    "-acpitable", f"file={BATAML}",
    "-cdrom", ISO,
    "-drive", f"file={DISK},format=raw,if=none,id=vda",
    "-device", "virtio-blk-pci,drive=vda",
    "-boot", "d",
    "-display", "none", "-vga", "std",
    "-serial", f"file:{SERIAL}",
    "-qmp", "unix:/tmp/q.sock,server=on,wait=off",
    "-monitor", "unix:/tmp/qm.sock,server=on,wait=off",
    "-netdev", "user,id=n0",
    "-device", "e1000,netdev=n0",
    "-rtc", "base=localtime",
    "-S",
    "-name", "Yart OS",
]
proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(f"[*] qemu pid {proc.pid}")

import json

def qmp_session():
    """Open ONE QMP connection, do capabilities, keep it open.  QEMU drops
    commands on connections that never sent qmp_capabilities."""
    for _ in range(30):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(15)
            s.connect("/tmp/q.sock")
            s.recv(4096)                                   # greeting
            s.sendall(b'{"execute":"qmp_capabilities"}\n')
            s.recv(4096)                                   # return
            return s
        except Exception as e:
            print("[!] qmp retry:", e)
            time.sleep(0.5)
    raise SystemExit("QMP never came up")

def qmp_cmd(s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        if b'"return"' in buf or b'"error"' in buf:
            break
    return buf.decode()

def hmp_send(line):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect("/tmp/qm.sock")
        s.sendall((line + "\n").encode())
        time.sleep(0.3)
        try:
            return s.recv(4096).decode()
        except socket.timeout:
            return ""

sess = qmp_session()
print("[*] qmp capabilities ok")
r = qmp_cmd(sess, {"execute": "cont"})
print("[*] cont:", r.strip()[:120])
print(f"[*] waiting {WAIT}s for boot + desktop ...")
time.sleep(WAIT)

r = hmp_send("screendump " + SHOT)
print("[*] screendump:", r.strip()[:60])

# convert
try:
    from PIL import Image
    im = Image.open(SHOT)
    im.save(PNG)
    print(f"[*] saved {PNG} ({im.size[0]}x{im.size[1]})")
except Exception as e:
    print("[!] convert failed:", e)

# serial log tail
if os.path.exists(SERIAL):
    txt = open(SERIAL, "rb").read().decode("latin1", "replace")
    print("=" * 60)
    print("SERIAL TAIL (last 2000 chars):")
    print(txt[-2000:])
    print("=" * 60)
    for key in ["acpi:", "battery", "SSDT", "wifi"]:
        for line in txt.splitlines():
            if key in line:
                print("LOG>", line.rstrip())
