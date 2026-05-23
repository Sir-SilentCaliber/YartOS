# Building Yart OS

This is the long-form build guide.  For TL;DR see the top-level `README.md`.

## 0.  Host requirements (Ubuntu 24.04 LTS)

You only really need three things:

```bash
sudo apt-get install -y build-essential bison flex libgmp3-dev libmpc-dev \
    libmpfr-dev texinfo wget xorriso nasm qemu-system-x86 ovmf mtools git
```

Yart compiles fine with the **Ubuntu-shipped** `x86_64-linux-gnu-gcc 14.2.0`
because we only ever use the freestanding subset (`-ffreestanding -nostdlib`
and friends).  However, the cross-compiled `x86_64-elf` toolchain is the
recommended path for hobby OS work because it physically cannot accidentally
link against host libraries.

## 1.  Cross compiler (~20 minutes, one time)

```bash
./scripts/build-toolchain.sh
export PATH="$HOME/opt/cross/bin:$PATH"
x86_64-elf-gcc --version    # confirm
```

This builds:

| Component | Version | Where |
|-----------|---------|-------|
| binutils  | 2.42    | `~/opt/cross` |
| gcc       | 14.2.0  | `~/opt/cross` |
| libgcc    | 14.2.0  | `~/opt/cross/lib/gcc/x86_64-elf/...` |

You can override the install prefix or version with env vars:

```bash
PREFIX=/opt/yart-cross GCC_VER=14.2.0 ./scripts/build-toolchain.sh
```

## 2.  Limine bootloader

```bash
./scripts/get-limine.sh
```

Pulls the v7.x binary branch and builds the `limine` host helper.  This is
a one-shot; the `Makefile` knows where to find the result.

## 3.  Build everything in parallel

```bash
make -j16 iso         # uses your AMD Ryzen 7 16 threads
```

Outputs:

* `build/yart.elf` - the kernel (higher-half x86_64 ELF, debug symbols on)
* `build/initrd.tar` - the YartFS initrd (USTAR)
* `iso_root/` - staging directory for the ISO
* `yart.iso` - the bootable hybrid ISO (BIOS + UEFI)

If you skip the cross compiler you can also do:

```bash
make CROSS=x86_64-linux-gnu -j16 iso
```

## 4.  Run in QEMU + KVM

```bash
make run
```

This invokes `scripts/run-qemu.sh`, which:

* enables KVM (`-enable-kvm -cpu host`)
* uses 4 vCPUs / 1 GiB
* boots `q35` with OVMF (UEFI) firmware
* attaches the ISO as `-cdrom`
* duplicates the kernel serial log to stdout (so you can debug-print)

If `/dev/kvm` is not accessible (e.g. running inside a CI container), the
script falls back to TCG and warns.

## 5.  Burn to USB

```bash
sudo ./scripts/usb-deploy.sh /dev/sdX
```

`/dev/sdX` must be the **whole device** (not `sdX1`).  The script lists the
device, asks for `YES` confirmation, unmounts any partitions, and then
runs `dd`.  Boots cleanly on the HP ProBook with the ProBook's UEFI menu
("F9 -> USB").

## 6.  Iterate

* Edit a `.c` -> `make -j16 iso`.  The dependency files (`*.d`) handle
  incremental rebuilds.
* Touch `kernel/linker.ld` -> the kernel ELF is re-linked.
* Touch `limine.cfg` or anything under `initrd_root/` -> the ISO is
  re-stamped.
* `make clean` nukes `build/`, `iso_root/`, and `yart.iso`.
* `make distclean` also removes the vendored `limine/`.

## 7.  Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `nasm: command not found` | host deps missing | `sudo apt install nasm` |
| `xorriso: command not found` | host deps missing | `sudo apt install xorriso` |
| OVMF missing | distro variant | `sudo apt install ovmf` and check `/usr/share/OVMF/` |
| Limine v7 helper missing `BOOTX64.EFI` | wrong branch | re-run `scripts/get-limine.sh` |
| Black screen in QEMU | VGA only | use `-vga std` (already in `run-qemu.sh`) |
| Triple-fault on real HW | wrong page-table bits | check `mm/vmm.c` flags, IST stacks |
