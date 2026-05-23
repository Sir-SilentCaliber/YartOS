#!/usr/bin/env bash
# Boot Yart in QEMU with KVM acceleration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/yart.iso"
[ -f "$ISO" ] || { echo "Build first: make iso"; exit 1; }

ACCEL="-enable-kvm -cpu host"
[ -w /dev/kvm ] || { echo "(no /dev/kvm, falling back to TCG)"; ACCEL="-cpu qemu64"; }

# Find OVMF firmware (path varies between Ubuntu/Debian releases)
OVMF=""
for cand in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/qemu/OVMF.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
    /usr/share/edk2/x64/OVMF_CODE.fd ; do
    if [ -f "$cand" ]; then OVMF="$cand"; break; fi
done

if [ -n "$OVMF" ]; then
    echo "Using UEFI firmware: $OVMF"
    FW=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF")
else
    echo "(!) No OVMF found - booting in legacy BIOS mode"
    FW=()
fi

exec qemu-system-x86_64 \
  $ACCEL \
  -smp 4 -m 1024 \
  -machine q35 \
  "${FW[@]}" \
  -cdrom "$ISO" \
  -boot d \
  -serial stdio \
  -vga std \
  -name "Yart OS"
