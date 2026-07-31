# =============================================================================
#  Yart OS - top-level Makefile
#  Usage:
#     make            -> build kernel
#     make iso        -> build kernel + bootable hybrid ISO (UEFI + BIOS)
#     make run        -> boot in QEMU with KVM
#     make clean
#  Parallel:  make -j16 iso
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

C_SRCS   := $(shell find kernel -name '*.c')
ASM_SRCS := $(shell find kernel -name '*.asm')
C_OBJS   := $(C_SRCS:%.c=build/%.o)
ASM_OBJS := $(ASM_SRCS:%.asm=build/%.o)
OBJS     := $(C_OBJS) $(ASM_OBJS)
DEPS     := $(C_OBJS:.o=.d)

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check       \
          -fno-pic -fno-pie -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -O2 -g -Wall -Wextra -Wno-unused-parameter             \
          -Wno-unused-function -Wno-address-of-packed-member                     \
          -Ikernel/include -MMD -MP                                              \
          -DYART_VERSION='"0.4.0-quartz"'

LDFLAGS := -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 \
           -T kernel/linker.ld

ASFLAGS := -f elf64 -Ikernel/arch/x86_64/

# Userland flags: small mcmodel, freestanding, link to fixed va.
UCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-sse -mno-mmx -mno-80387 -O2 -Wall -Wextra
ULDFLAGS := -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 \
            -T userland/init.ld

.PHONY: all iso run clean distclean limine
all: $(KERNEL) $(USER_ELF)

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
build/init.o: userland/init.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/init.c -o build/init.o

$(USER_ELF): build/init.o userland/init.ld
	$(LD) $(ULDFLAGS) build/init.o -o $@

initrd_root/bin/init: $(USER_ELF)
	@mkdir -p initrd_root/bin
	cp $(USER_ELF) initrd_root/bin/init

# ---- Limine ----
$(LIMINE):
	./scripts/get-limine.sh

# ---- Initrd ----
build/initrd.tar: initrd_root/etc/motd initrd_root/etc/yart.conf \
                  initrd_root/etc/wallpaper.bmp \
                  initrd_root/bin/init
	@mkdir -p build
	cd initrd_root && tar --format=ustar -cf ../build/initrd.tar .

initrd_root/etc/motd:
	@mkdir -p initrd_root/etc
	@printf "Yart OS - the calm, simple desktop.\n" > $@

initrd_root/etc/yart.conf:
	@mkdir -p initrd_root/etc
	@printf "hostname=yart\ntheme=slate-amber\naccent=#E8A87C\nborder=#E8A87C\ncorner_radius=6\ndock.position=bottom\ndock.icon_size=32\ndock.pinned=Files,Term,Editor,Calc,Mon\nwallpaper.mode=image\nwallpaper.path=/etc/wallpaper.bmp\nfont.system=default\nfont.terminal=default\ndisplay.fps=30\ntime.format24=1\n" > $@

# ---- ISO ----
iso: $(ISO)

$(ISO): $(KERNEL) build/initrd.tar $(LIMINE)
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL)              $(ISO_ROOT)/boot/yart.elf
	cp build/initrd.tar       $(ISO_ROOT)/boot/initrd.tar
	cp limine.cfg             $(ISO_ROOT)/boot/limine/limine.cfg
	cp $(LIMINE)/limine-bios.sys           $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin        $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin        $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI               $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin                  \
	    -no-emul-boot -boot-load-size 4 -boot-info-table                   \
	    --efi-boot boot/limine/limine-uefi-cd.bin                          \
	    -efi-boot-part --efi-boot-image --protective-msdos-label           \
	    $(ISO_ROOT) -o $(ISO)
	$(LIMINE)/limine bios-install $(ISO)

run: $(ISO)
	./scripts/run-qemu.sh

run-bios: $(ISO)
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 1024 \
	    -cdrom $(ISO) -boot d -serial stdio

clean:
	rm -rf build $(ISO_ROOT) $(ISO) initrd_root/bin/init

distclean: clean
	rm -rf $(LIMINE)

-include $(DEPS)

initrd_root/etc/wallpaper.bmp: scripts/gen_wallpaper_bmp.py
	@mkdir -p initrd_root/etc
	python3 scripts/gen_wallpaper_bmp.py $@

# Regenerate font and asset tables when their scripts change
kernel/gui/font_data.c: scripts/gen_fonts.py
	python3 scripts/gen_fonts.py $@

kernel/gui/asset_icons.c kernel/gui/asset_cursor.c kernel/gui/asset_wallpaper.c: scripts/gen_assets.py scripts/gen_assets_win.py
	python3 scripts/gen_assets.py

