#!/usr/bin/env bash
# Boot Yart in QEMU with KVM acceleration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/yart.iso"
[ -f "$ISO" ] || { echo "Build first: make iso"; exit 1; }

ACCEL="-enable-kvm -cpu host"
if [ -w /dev/kvm ]; then
    echo "[OK] KVM acceleration detected - smooth desktop."
else
    cat >&2 <<'EOF'

=========================================================================
 WARNING: no /dev/kvm - falling back to TCG (pure software emulation).
 TCG is 10-50x SLOWER than KVM. The desktop (especially the cursor) WILL
 feel laggy no matter how optimized the code is. This is NOT a YartOS bug.

 To fix (pick one):
   1. Enable KVM:  sudo modprobe kvm-intel   (or kvm-amd)
                   sudo usermod -aG kvm $USER   (then log out/in)
      Re-run and confirm:  ls -l /dev/kvm
   2. Or boot the ISO on real hardware (USB):  scripts/usb-deploy.sh
=========================================================================

EOF
    ACCEL="-cpu qemu64"
fi

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

# Virtual disk for persistent storage (created on first run)
DISK="$ROOT/yart-disk.img"
if [ ! -f "$DISK" ]; then
    echo "(!) creating 64 MiB virtual disk: $DISK"
    dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
fi

exec qemu-system-x86_64 \
  $ACCEL \
  -smp 4 -m 1024 \
  -machine q35 \
  "${FW[@]}" \
  -acpitable file="$ROOT/acpi/battery.aml" \
  -cdrom "$ISO" \
  -drive "file=$DISK,format=raw,if=none,id=vda" \
  -device virtio-blk-pci,drive=vda \
  -boot d \
  -serial stdio \
  -vga std \
  -name "Yart OS"
