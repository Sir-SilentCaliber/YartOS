#!/usr/bin/env bash
# usb-deploy.sh : burn yart.iso to a USB stick.  DESTROYS THE TARGET.
#
#   sudo ./scripts/usb-deploy.sh /dev/sdX
#
set -euo pipefail
if [ "$#" -ne 1 ]; then
  echo "Usage: sudo $0 /dev/sdX  (NOT a partition like sdX1)"
  exit 1
fi
DEV="$1"
ISO="$(dirname "$0")/../yart.iso"
[ -b "$DEV" ] || { echo "Not a block device: $DEV"; exit 1; }
[ -f "$ISO" ] || { echo "Build the ISO first: make iso"; exit 1; }

echo "About to wipe $DEV and write $ISO"
lsblk "$DEV"
read -rp "Type YES to continue: " CONFIRM
[ "$CONFIRM" = "YES" ] || { echo "Aborted."; exit 1; }

# Unmount any auto-mounted partitions
for p in $(lsblk -nrpo NAME "$DEV" | tail -n +2); do
  umount "$p" 2>/dev/null || true
done

dd if="$ISO" of="$DEV" bs=4M status=progress oflag=sync conv=fdatasync
sync
echo "Done. Eject and boot your HP ProBook from USB."
