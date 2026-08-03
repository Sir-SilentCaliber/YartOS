# =============================================================================
#  Yart OS - top-level Makefile
# =============================================================================

CROSS    ?= x86_64-elf
ifeq ($(shell which $(CROSS)-gcc 2>/dev/null),)
  CC      := gcc
  LD      := ld
  OBJCOPY := objcopy
else
  CC      := $(CROSS)-gcc
  LD      := $(CROSS)-ld
  OBJCOPY := $(CROSS)-objcopy
endif
AS       := nasm

KERNEL   := build/yart.elf
USER_ELF := build/init.elf
ISO      := yart.iso
ISO_ROOT := iso_root
LIMINE   := limine

ICON_DIR := kora/icons
WP_DIR   := kora/wallpapers
KORA_BIN := build/kora.bin
KORA_H   := build/kora.h
WP_BIN   := build/wallpaper.bin
WP_PNG   := $(WP_DIR)/default.png
# Asset layout: /YartOS/kora/ is the canonical location for shipped assets.
# /etc is reserved for user-editable config (yart.conf, motd).
BMP_WALL := initrd_root/YartOS/kora/wallpaper.bmp

C_SRCS   := $(shell find kernel -name '*.c')
ASM_SRCS := $(shell find kernel -name '*.asm' ! -name 'smp_tramp.asm')
C_OBJS   := $(C_SRCS:%.c=build/%.o)
ASM_OBJS := $(ASM_SRCS:%.asm=build/%.o)
OBJS     := $(C_OBJS) $(ASM_OBJS)
DEPS     := $(C_OBJS:.o=.d)

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check       \
          -fno-pic -fno-pie -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -O2 -g -Wall -Wextra -Wno-unused-parameter             \
          -Wno-unused-function -Wno-address-of-packed-member                     \
          -Ikernel/include -MMD -MP                                              \
          -DYART_VERSION='"0.5.0-quartz"'

LDFLAGS := -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 \
           -T kernel/linker.ld

ASFLAGS := -f elf64 -Ikernel/arch/x86_64/

UCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -fPIE \
           -mno-red-zone -O3 -Wall -Wextra -Iuserland -Ibuild
ULDFLAGS := -nostdlib -static -pie -m elf_x86_64 -z max-page-size=0x1000 \
            -T userland/init.ld

# ---- Host-tool checks ----
REQUIRED_TOOLS := nasm xorriso git python3
MISSING_TOOLS := $(strip $(foreach t,$(REQUIRED_TOOLS),$(if $(shell which $(t) 2>/dev/null),,$(t))))
ifneq ($(MISSING_TOOLS),)
  $(info ERROR: missing required host tools: $(MISSING_TOOLS))
  $(info Install with (Debian/Ubuntu):)
  $(info   sudo apt install -y build-essential nasm xorriso git python3 python3-pil librsvg2-bin qemu-system-x86 ovmf)
  $(error Aborting)
endif
# rsvg-convert is in librsvg2-bin
ifeq ($(shell which rsvg-convert 2>/dev/null),)
  $(info ERROR: rsvg-convert (librsvg2-bin) not found.)
  $(info   Debian/Ubuntu: sudo apt install librsvg2-bin)
  $(error Aborting)
endif
# qemu-system-x86_64 (only needed for `make run`; skip check for plain `make iso`)
# Check is deferred to the `run` target so `make iso` works on headless build boxes.
# python3 PIL check via a quick inline import
PIL_OK := $(strip $(shell python3 -c "from PIL import Image" 2>/dev/null && echo OK))
ifneq ($(PIL_OK),OK)
  $(info ERROR: Python Pillow module not found.)
  $(info   pip3 install Pillow     -- or --     sudo apt install python3-pil)
  $(error Aborting)
endif

.PHONY: all iso run clean distclean limine assets
all: $(KERNEL) $(USER_ELF)

assets: $(KORA_BIN) $(KORA_H) $(WP_BIN)

# ---- Icon pack (Kora SVGs -> RGBA atlas) ----
$(KORA_BIN) $(KORA_H): scripts/gen_assets.py
	@mkdir -p build
	@chmod +x scripts/gen_assets.py 2>/dev/null || true
	python3 scripts/gen_assets.py
	@test -s $(KORA_BIN) || { echo "ERROR: kora.bin missing after asset build — rsvg-convert probably failed."; exit 1; }

# ---- Wallpaper pack (multiple BGRA wallpapers linked into compositor) ----
# scripts/gen_wallpaper_pack.py generates all wallpapers procedurally, writes
# PNGs to kora/wallpapers/*.png AND the pack binary build/wallpaper.bin.
# Format v2: magic + offset table so the compositor can cycle through them.
$(WP_BIN): scripts/gen_wallpaper_pack.py
	@mkdir -p build $(WP_DIR)
	@chmod +x scripts/gen_wallpaper_pack.py 2>/dev/null || true
	python3 scripts/gen_wallpaper_pack.py
	@test -s $(WP_BIN) || { echo "ERROR: wallpaper.bin missing after asset build."; exit 1; }

$(WP_PNG): $(WP_BIN)
	@: default.png is produced as a side-effect of the pack step above

$(BMP_WALL): scripts/gen_wallpaper_bmp.py $(WP_PNG)
	@mkdir -p initrd_root/YartOS/kora
	@chmod +x scripts/gen_wallpaper_bmp.py 2>/dev/null || true
	python3 scripts/gen_wallpaper_bmp.py $(BMP_WALL)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# ---- Userland ----
build/start.o: userland/start.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/start.c -o build/start.o
build/init.o: userland/init.c userland/sys.h userland/wm.c userland/gfx.h $(KORA_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/init.c -o build/init.o
build/wm.o: userland/wm.c userland/sys.h userland/gfx.h $(KORA_H) assets
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/wm.c -o build/wm.o
build/gfx.o: userland/gfx.c userland/gfx.h userland/sys.h $(KORA_H) assets
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/gfx.c -o build/gfx.o
build/assets.o: $(KORA_BIN)
	@mkdir -p build
	cd build && $(LD) -r -b binary -o assets.o kora.bin
build/wallpaper.o: $(WP_BIN)
	@mkdir -p build
	cd build && $(LD) -r -b binary -o wallpaper.o wallpaper.bin

$(USER_ELF): build/start.o build/init.o build/wm.o build/gfx.o build/assets.o build/wallpaper.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/init.o build/wm.o build/gfx.o build/assets.o build/wallpaper.o -o $@

# ---- /bin/hello: the exec() demo binary ----
build/hello.o: userland/hello.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/hello.c -o build/hello.o
build/hello.elf: build/start.o build/hello.o userland/init.ld
	@mkdir -p build
	$(LD) $(ULDFLAGS) build/start.o build/hello.o -o $@

initrd_root/bin/init: $(USER_ELF)
	@mkdir -p initrd_root/bin
	cp $(USER_ELF) initrd_root/bin/init

initrd_root/bin/hello: build/hello.elf
	@mkdir -p initrd_root/bin
	cp build/hello.elf initrd_root/bin/hello

initrd_root/etc/motd:
	@mkdir -p initrd_root/etc
	@printf "Yart OS - the calm, simple desktop.\n" > $@

initrd_root/etc/yart.conf:
	@mkdir -p initrd_root/etc
	@printf "hostname=yart\ntheme=quartz\naccent=#5BA7DF\nborder=#5BA7DF\ncorner_radius=8\ndock.position=bottom\ndock.icon_size=34\ndock.spacing=44\nwallpaper.mode=image\nwallpaper.path=/YartOS/kora/wallpaper.bmp\nwallpaper.index=0\nfont.system=default\nfont.terminal=default\ndisplay.fps=60\ntime.format24=1\ncursor.size=24\n" > $@

# ---- Limine (fetched on demand) ----
$(LIMINE):
	@chmod +x scripts/get-limine.sh 2>/dev/null || true
	bash ./scripts/get-limine.sh

# ---- Initrd ----
build/initrd.tar: initrd_root/etc/motd initrd_root/etc/yart.conf $(BMP_WALL) initrd_root/bin/init initrd_root/bin/hello
	@mkdir -p build
	cd initrd_root && tar --format=ustar -cf ../build/initrd.tar .

# ---- ISO ----
iso: $(ISO)

$(ISO): $(KERNEL) build/initrd.tar $(LIMINE)
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL)              $(ISO_ROOT)/boot/yart.elf
	cp build/initrd.tar       $(ISO_ROOT)/boot/initrd.tar
	cp limine.cfg             $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-bios.sys           $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin        $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin        $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI               $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin                  \
	    -no-emul-boot -boot-load-size 4 -boot-info-table                   \
	    --efi-boot boot/limine/limine-uefi-cd.bin                          \
	    -efi-boot-part --efi-boot-image --protective-msdos-label           \
	    $(ISO_ROOT) -o $(ISO)
	@chmod +x $(LIMINE)/limine
	$(LIMINE)/limine bios-install $(ISO)

run: $(ISO)
	@if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then \
	  echo "ERROR: qemu-system-x86_64 not found (needed for make run)."; \
	  echo "  Debian/Ubuntu: sudo apt install qemu-system-x86 ovmf"; exit 1; fi
	@chmod +x scripts/run-qemu.sh 2>/dev/null || true
	bash ./scripts/run-qemu.sh

run-bios: $(ISO)
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 1024 \
	    -cdrom $(ISO) -boot d -serial stdio

clean:
	rm -rf build $(ISO_ROOT) $(ISO) initrd_root/bin/init initrd_root/bin/hello \
	           $(BMP_WALL) initrd_root/etc/motd initrd_root/etc/yart.conf \
	           initrd_root/YartOS
distclean: clean
	rm -rf $(LIMINE)

-include $(DEPS)
