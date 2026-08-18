#!/usr/bin/env python3
"""Regression: lock screen requires the real password (yart), rejects wrong."""
import socket, time, subprocess, os, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
subprocess.run(["pkill","-9","qemu-system"], capture_output=True); time.sleep(0.3)
for p in ["/tmp/q.sock","/tmp/qm.sock"]:
    try: os.unlink(p)
    except FileNotFoundError: pass
subprocess.Popen([
    "qemu-system-x86_64","-cpu","max","-smp","4","-m","1024","-machine","q35",
    "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-cdrom",f"{ROOT}/yart.iso","-drive",f"file={ROOT}/yart-disk.img,format=raw,if=virtio",
    "-display","none","-vga","std","-rtc","base=localtime","-serial",f"file:{ROOT}/runlogs/serial.log",
    "-qmp","unix:/tmp/q.sock,server=on,wait=off","-monitor","unix:/tmp/qm.sock,server=on,wait=off",
    "-S"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        s=socket.socket(socket.AF_UNIX); s.connect("/tmp/q.sock"); break
    except OSError: time.sleep(0.1)
s.settimeout(5)
def r():
    b=b""
    while not b.endswith(b"\n"): b+=s.recv(4096)
    return b
r(); s.sendall(b'{"execute":"qmp_capabilities"}\n'); r(); s.sendall(b'{"execute":"cont"}\n'); r()
def ev(name, down):
    s.sendall(json.dumps({"execute":"input-send-event","arguments":{"events":[
        {"type":"key","data":{"key":{"type":"qcode","data":name},"down":down}}]}}).encode()+b"\n")
    r(); time.sleep(0.05)
def tap(name):
    ev(name, True); time.sleep(0.06); ev(name, False); time.sleep(0.10)
def type_str(st):
    for ch in st: tap(ch)

time.sleep(20)
h=socket.socket(socket.AF_UNIX); h.connect("/tmp/qm.sock"); h.settimeout(5); time.sleep(0.3)
def shot(name): h.sendall(f"screendump {ROOT}/{name}.ppm\n".encode()); time.sleep(0.4)
shot("lk0")
ev("meta_l", True); time.sleep(0.1); ev("l", True); time.sleep(0.1); ev("l", False); ev("meta_l", False)
time.sleep(0.7); shot("lk1")
tap("ret"); time.sleep(0.4); shot("lk2")
type_str("wrong"); tap("ret"); time.sleep(0.6); shot("lk3")
type_str("yart"); tap("ret"); time.sleep(0.8); shot("lk4")
h.close(); s.close(); print("done")
