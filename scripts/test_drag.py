#!/usr/bin/env python3
"""QEMU smoke test for dragging a window by its title bar.

Uses an isolated temporary disk/socket/log and only terminates the QEMU
process it starts. It assumes the 1280x800 test desktop and centered pointer.
"""
import json
import os
import socket
import subprocess
import tempfile
import time

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    iso = os.path.join(ROOT, "yart.iso")
    if not os.path.isfile(iso):
        raise SystemExit("Build first: make iso")

    with tempfile.TemporaryDirectory(prefix="yart-drag-") as tmp:
        qmp_path = os.path.join(tmp, "qmp.sock")
        serial_path = os.path.join(tmp, "serial.log")
        disk_path = os.path.join(tmp, "disk.img")
        subprocess.run(["truncate", "-s", "64M", disk_path], check=True)
        command = [
            "qemu-system-x86_64", "-cpu", "qemu64", "-smp", "4", "-m", "1024",
            "-machine", "q35", "-cdrom", iso,
            "-drive", f"file={disk_path},format=raw,if=virtio",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-display", "none", "-vga", "std", "-rtc", "base=localtime",
            "-serial", f"file:{serial_path}",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-no-reboot", "-S",
        ]
        proc = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
        )
        qmp = None
        try:
            print("[*] Waiting for QEMU...")
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited early: {proc.stderr.read()}")
                try:
                    qmp = socket.socket(socket.AF_UNIX)
                    qmp.settimeout(5)
                    qmp.connect(qmp_path)
                    break
                except OSError:
                    if qmp:
                        qmp.close()
                    qmp = None
                    time.sleep(0.1)
            if qmp is None:
                raise TimeoutError("QEMU did not open its QMP socket")

            reader = qmp.makefile("r", encoding="utf-8")
            greeting = json.loads(reader.readline())
            if "QMP" not in greeting:
                raise RuntimeError("QEMU did not send a QMP greeting")

            def qmp_cmd(execute, arguments=None):
                request = {"execute": execute}
                if arguments is not None:
                    request["arguments"] = arguments
                qmp.sendall(json.dumps(request).encode() + b"\n")
                for line in reader:
                    data = json.loads(line)
                    if "return" in data:
                        return data["return"]
                    if "error" in data:
                        raise RuntimeError(f"QMP {execute} failed: {data['error']}")
                raise RuntimeError(f"QMP disconnected during {execute}")

            qmp_cmd("qmp_capabilities")
            qmp_cmd("cont")

            def key_event(name, down):
                qmp_cmd("input-send-event", {
                    "events": [{
                        "type": "key",
                        "data": {"key": {"type": "qcode", "data": name}, "down": down},
                    }]
                })
                time.sleep(0.05)

            def tap(name):
                key_event(name, True)
                key_event(name, False)

            def type_text(text):
                for char in text:
                    tap("spc" if char == " " else "ret" if char == "\n" else char)

            def mouse_move(dx, dy):
                qmp_cmd("input-send-event", {
                    "events": [
                        {"type": "rel", "data": {"axis": "x", "value": dx}},
                        {"type": "rel", "data": {"axis": "y", "value": dy}},
                    ]
                })
                time.sleep(0.08)

            def mouse_button(button, down):
                qmp_cmd("input-send-event", {
                    "events": [{"type": "btn", "data": {"button": button, "down": down}}]
                })
                time.sleep(0.08)

            print("[*] Waiting for desktop...")
            booted = False
            for _ in range(120):
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited during boot: {proc.stderr.read()}")
                try:
                    with open(serial_path, encoding="utf-8", errors="replace") as log:
                        if "wallpaper loaded successfully" in log.read():
                            booted = True
                            break
                except FileNotFoundError:
                    pass
                time.sleep(0.5)
            if not booted:
                raise TimeoutError("YartOS did not reach the desktop; inspect the QEMU serial log")
            time.sleep(2)

            # Unlock the factory demo session and open Nyra.
            tap("ret")
            time.sleep(0.3)
            type_text("yart\n")
            time.sleep(1.5)
            key_event("meta_l", True)
            tap("t")
            key_event("meta_l", False)
            time.sleep(2)

            # The missing package must fail closed, not elevate the shell or
            # start a connection to the development-only repository.
            with open(serial_path, encoding="utf-8", errors="replace") as log:
                serial_before = log.read()
            type_text("apk add discord\n")
            time.sleep(1)
            if proc.poll() is not None:
                raise AssertionError("QEMU exited after apk add discord")
            with open(serial_path, encoding="utf-8", errors="replace") as log:
                serial_after = log.read()
            serial_delta = serial_after[len(serial_before):]
            if "elevated to root via doas" in serial_delta:
                raise AssertionError("apk add unexpectedly elevated Nyra to root")
            if "tcp: connecting to 10.0.2.2:8000" in serial_delta:
                raise AssertionError("apk add contacted the unavailable dev repository")
            print("[PASS] apk add discord stayed unprivileged and did not contact the dev repository")
            time.sleep(0.5)

            def screenshot(filename):
                path = os.path.join(tmp, filename)
                qmp_cmd("screendump", {"filename": path})
                with Image.open(path) as image:
                    return image.convert("RGB").copy()

            # Park the cursor outside the window so the before/after captures
            # differ because of the drag, not because the cursor moved.
            mouse_move(-410, -320)   # centered (640,400) -> neutral (230,80)
            time.sleep(0.4)
            before = screenshot("before.ppm")

            # Console title bar is at approximately (430,180). Drag it +100px.
            mouse_move(200, 100)
            mouse_button("left", True)
            for _ in range(10):
                mouse_move(10, 10)
            mouse_button("left", False)
            time.sleep(0.8)
            mouse_move(-300, -200)   # return cursor to the same neutral point
            time.sleep(0.4)
            after = screenshot("after.ppm")

            if before.size != after.size:
                raise AssertionError("screen size changed during drag test")
            delta = ImageChops.difference(before.crop((280, 130, 700, 360)),
                                          after.crop((280, 130, 700, 360))).convert("L")
            changed_pixels = sum(pixel > 24 for pixel in delta.getdata())
            if changed_pixels < 2000:
                raise AssertionError(
                    f"title-bar drag did not visibly move the window "
                    f"({changed_pixels} changed pixels)"
                )
            print(f"[PASS] title-bar drag changed {changed_pixels} pixels in the test region")
        finally:
            if qmp:
                qmp.close()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


if __name__ == "__main__":
    main()
