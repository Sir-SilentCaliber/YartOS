#!/usr/bin/env bash
set -euo pipefail
# Portable: resolve the repo root from this script's location, not a hardcoded path.
ROOT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ROOT/runlogs"
rm -f "$ROOT/audio-out.wav"
pkill -9 qemu-system 2>/dev/null || true
sleep 0.3

# Find OVMF firmware (path varies between Ubuntu/Debian releases)
OVMF=""
for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/ovmf/OVMF.fd /usr/share/edk2/x64/OVMF_CODE.fd ; do
    if [ -f "$c" ]; then OVMF="$c"; break; fi
done
[ -n "$OVMF" ] || { echo "(!) no OVMF firmware found"; exit 1; }
rm -f /tmp/qemu-qmp.sock

# Create the persistent disk on first run (local artifact, not in the tree)
[ -f "$ROOT/yart-disk.img" ] || dd if=/dev/zero of="$ROOT/yart-disk.img" bs=1M count=64 status=none

# Hardware acceleration when available: KVM is ~10-50x faster than pure TCG
# emulation.  TCG is why the desktop can feel "heavy"/laggy on some machines.
ACCEL=(-cpu max)
if [ -w /dev/kvm ]; then
    ACCEL=(-enable-kvm -cpu host)
    echo "run: KVM acceleration enabled"
else
    echo "run: no /dev/kvm - falling back to TCG emulation (slower)"
fi

exec qemu-system-x86_64 \
  "${ACCEL[@]}" \
  -smp 4 -m 1024 \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
  -acpitable file="$ROOT/acpi/battery.aml" \
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
  -rtc base=localtime \
  -name "Yart OS"
