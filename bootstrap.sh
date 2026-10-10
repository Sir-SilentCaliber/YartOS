#!/usr/bin/env bash
# bootstrap.sh — install host build dependencies on common distros.
# Usage: ./bootstrap.sh         (auto-detects distro)
set -euo pipefail

echo "Yart OS — host dependency bootstrap"
echo

install_debian() {
    echo "[*] Debian/Ubuntu detected — installing build deps with apt..."
    sudo apt update
    sudo apt install -y build-essential nasm xorriso git python3 python3-pil \
                        librsvg2-bin qemu-system-x86 ovmf
}

install_arch() {
    echo "[*] Arch detected — installing build deps with pacman..."
    sudo pacman -Sy --needed base-devel nasm xorriso git python python-pillow \
                            librsvg qemu-system-x86 edk2-ovmf
}

install_fedora() {
    echo "[*] Fedora detected — installing build deps with dnf..."
    sudo dnf install -y @development-tools nasm xorriso git python3 python3-pillow \
                        librsvg2-tools qemu-system-x86 edk2-ovmf
}

install_macos() {
    echo "[*] macOS detected — installing build deps with Homebrew..."
    if ! command -v brew >/dev/null; then
        echo "Install Homebrew first: https://brew.sh"; exit 1
    fi
    brew install nasm xorriso git python3 qemu librsvg
    pip3 install Pillow
    echo "NOTE: OVMF (UEFI firmware) not installed via brew — BIOS boot will work."
}

if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian|linuxmint|pop|elementary|zorin|kali) install_debian ;;
        arch|manjaro|endeavouros)                         install_arch ;;
        fedora|rhel|centos|rocky|alma)                    install_fedora ;;
        *) echo "Unrecognized distro: $ID — see README.md for manual deps."; exit 1 ;;
    esac
elif [ "$(uname)" = "Darwin" ]; then
    install_macos
else
    echo "Can't detect your OS. Install these manually:"
    echo "  - C compiler (gcc/clang), GNU make, nasm, xorriso, git"
    echo "  - python3 + Pillow (python3-pil)"
    echo "  - rsvg-convert (librsvg2-bin / librsvg)"
    echo "  - qemu-system-x86 (+ OVMF for UEFI)"
    exit 1
fi

echo
echo "All dependencies installed. Try:  make -j\$(nproc) iso && ./scripts/run-qemu.sh"
