# Yart OS — reproducible build container.
#
# Builds the whole OS in a pinned Debian image so anyone (Linux/macOS/Windows
# with Docker) gets a byte-for-byte reproducible toolchain and build, no
# matter what's installed on their host.
#
# Usage:
#   docker build -t yartos .                 # build the toolchain image
#   docker run --rm -v "$PWD":/src yartos make -j"$(nproc)" iso
#
# (The second command builds ./yart.iso into your host checkout.)
FROM debian:12

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential nasm xorriso git python3 python3-pil librsvg2-bin \
        qemu-system-x86 ovmf ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
ENTRYPOINT ["make"]
