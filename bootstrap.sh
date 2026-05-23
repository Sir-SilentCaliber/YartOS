#!/usr/bin/env bash
# bootstrap.sh - one-shot setup + build + run for Yart OS.
#
# Run from inside the yart/ directory:
#     chmod +x bootstrap.sh
#     ./bootstrap.sh            # builds toolchain (if missing), limine, ISO, then runs QEMU
#     ./bootstrap.sh fast       # SKIP cross-compiler; use host x86_64-linux-gnu-gcc
#     ./bootstrap.sh iso        # build ISO only (no run)
#     ./bootstrap.sh usb /dev/sdX   # burn to USB
#
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-full}"

step() { printf "\n\033[1;36m==>\033[0m %s\n" "$*"; }

# ---- 0. host deps ----
step "Installing host dependencies (sudo)"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential nasm xorriso qemu-system-x86 ovmf mtools git \
    bison flex libgmp-dev libmpc-dev libmpfr-dev texinfo wget \
    imagemagick

# ---- 1. toolchain ----
if [ "$MODE" = "fast" ]; then
    CROSS=x86_64-linux-gnu
    step "Using host toolchain ($CROSS-gcc)"
else
    if ! command -v x86_64-elf-gcc >/dev/null 2>&1 \
       && [ ! -x "$HOME/opt/cross/bin/x86_64-elf-gcc" ]; then
        step "Building x86_64-elf cross compiler (~20 min, one time)"
        ./scripts/build-toolchain.sh
    fi
    export PATH="$HOME/opt/cross/bin:$PATH"
    CROSS=x86_64-elf
    step "Using cross toolchain ($CROSS-gcc)"
fi

# ---- 2. Limine ----
if [ ! -d limine ]; then
    step "Fetching Limine bootloader"
    ./scripts/get-limine.sh
fi

# ---- 3. build ISO ----
step "Building Yart kernel + ISO ($(nproc) jobs)"
make CROSS="$CROSS" -j"$(nproc)" iso

# ---- 4. action ----
case "$MODE" in
  iso)
    step "Done. yart.iso is at $(pwd)/yart.iso"
    ;;
  usb)
    DEV="${2:-}"
    [ -n "$DEV" ] || { echo "Usage: $0 usb /dev/sdX"; exit 1; }
    step "Burning to $DEV"
    sudo ./scripts/usb-deploy.sh "$DEV"
    ;;
  *)
    step "Booting in QEMU (Ctrl+Alt+G to release mouse, close window to quit)"
    ./scripts/run-qemu.sh
    ;;
esac
