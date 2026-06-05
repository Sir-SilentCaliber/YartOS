#!/usr/bin/env bash
# build-toolchain.sh : Build an x86_64-elf GCC cross-compiler for Yart OS
#
# Tested on Ubuntu 24.04. Installs to $HOME/opt/cross.
# Run ONCE.  After that, just:  export PATH="$HOME/opt/cross/bin:$PATH"
#
set -euo pipefail

PREFIX="${PREFIX:-$HOME/opt/cross}"
TARGET="${TARGET:-x86_64-elf}"
JOBS="${JOBS:-$(nproc)}"
BINUTILS_VER="2.42"
GCC_VER="14.2.0"

WORK="$HOME/src/yart-toolchain"
mkdir -p "$WORK" "$PREFIX"
cd "$WORK"

echo "==> Installing host build deps (sudo required)"
sudo apt-get update
sudo apt-get install -y build-essential bison flex libgmp3-dev libmpc-dev \
    libmpfr-dev texinfo wget xorriso nasm qemu-system-x86 ovmf mtools

if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
  wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
fi
if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
  wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
fi

[ -d "binutils-${BINUTILS_VER}" ] || tar xf "binutils-${BINUTILS_VER}.tar.xz"
[ -d "gcc-${GCC_VER}" ]           || tar xf "gcc-${GCC_VER}.tar.xz"

echo "==> Building binutils"
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../binutils-${BINUTILS_VER}/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS"
make install
cd ..

echo "==> Building GCC (c, no libc)"
cd "gcc-${GCC_VER}"
contrib/download_prerequisites
cd ..
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../gcc-${GCC_VER}/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers \
    --disable-hosted-libstdcxx
make -j"$JOBS" all-gcc
make -j"$JOBS" all-target-libgcc
make install-gcc
make install-target-libgcc

echo
echo "Done. Add this to your shell rc:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
echo "Verify:  ${TARGET}-gcc --version"
