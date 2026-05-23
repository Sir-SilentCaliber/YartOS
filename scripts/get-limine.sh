#!/usr/bin/env bash
# Fetch Limine v7.x binary branch and build the host helper `limine`
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [ ! -d limine ]; then
  git clone --depth=1 --branch=v7.x-binary https://github.com/limine-bootloader/limine.git
fi
make -C limine
echo "Limine ready at $ROOT/limine"
