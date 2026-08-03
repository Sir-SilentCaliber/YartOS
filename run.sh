#!/usr/bin/env bash
set -euo pipefail
ROOT=/home/user/YartOS
mkdir -p "$ROOT/runlogs"
rm -f "$ROOT/audio-out.wav"
pkill -9 qemu-system 2>/dev/null || true
sleep 0.3

OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
rm -f /tmp/qemu-qmp.sock

exec qemu-system-x86_64 \
  -cpu max \
  -smp 4 -m 1024 \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
  -cdrom "$ROOT/yart.iso" \
  -drive file="$ROOT/yart-disk.img",format=raw,if=none,id=vda \
  -device virtio-blk-pci,drive=vda \
  -boot d \
  -display none \
  -vga std \
  -chardev file,id=ser,path="$ROOT/runlogs/serial.log" \
  -serial chardev:ser \
  -qmp unix:/tmp/qemu-qmp.sock,server,nowait \
  -audiodev wav,id=wav0,path="$ROOT/audio-out.wav" \
  -device intel-hda -device hda-duplex,audiodev=wav0 \
  -netdev user,id=n0 \
  -device e1000,netdev=n0 \
  -usb -device qemu-xhci -device usb-kbd -device usb-mouse \
  -name "Yart OS"
