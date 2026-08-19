# =============================================================================
#  Yart OS - top-level Makefile v3 MAX
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
WM_ELF   := build/wm.elf
ISO      := yart.iso
ISO_ROOT := iso_root
LIMINE   := limine

ICON_DIR := kora/icons
WP_DIR   := kora/wallpapers
KORA_BIN := build/kora.bin
KORA_H   := build/kora.h
WP_BIN   := build/wallpaper.bin
WP_PNG   := $(WP_DIR)/default.png
CUR_BIN  := build/cursors.bin
CUR_H    := build/cursor_assets.h
BMP_WALL := initrd_root/YartOS/kora/wallpaper.bmp

# Portable source enumeration. Some hosts/distros have a non-GNU or oddly
# configured `find` in PATH; $(wildcard ...) is built into make and cannot
# silently produce an empty kernel object list.
KERNEL_C_SRCS := \
  $(wildcard kernel/*.c) \
  $(wildcard kernel/arch/*/*.c) \
  $(wildcard kernel/drivers/*.c) \
  $(wildcard kernel/fs/*.c) \
  $(wildcard kernel/gui/*.c) \
  $(wildcard kernel/lib/*.c) \
  $(wildcard kernel/mm/*.c) \
  $(wildcard kernel/net/*.c) \
  $(wildcard kernel/sched/*.c)
KERNEL_ASM_SRCS := $(wildcard kernel/arch/*/*.asm)
# smp_tramp.asm is included/generated separately; do not link it directly.
KERNEL_ASM_SRCS := $(filter-out %/smp_tramp.asm,$(KERNEL_ASM_SRCS))
C_OBJS   := $(patsubst %.c,build/%.o,$(KERNEL_C_SRCS))
ASM_OBJS := $(patsubst %.asm,build/%.o,$(KERNEL_ASM_SRCS))
OBJS     := $(C_OBJS) $(ASM_OBJS)
DEPS     := $(C_OBJS:.o=.d)

ifeq ($(strip $(KERNEL_C_SRCS)),)
  $(error ERROR: no kernel C sources found under kernel/ -- check the repository checkout)
endif
ifeq ($(strip $(OBJS)),)
  $(error ERROR: kernel object list is empty; cannot link build/yart.elf)
endif

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check       \
          -fno-pic -fno-pie -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -O2 -g -Wall -Wextra -Wno-unused-parameter             \
          -Wno-unused-function -Wno-address-of-packed-member                     \
          -Ikernel/include -MMD -MP                                              \
          -DYART_VERSION='"0.8.0-max"'

LDFLAGS := -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 \
           -T kernel/linker.ld

ASFLAGS := -f elf64 -Ikernel/arch/x86_64/

UCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -fPIE -msse2 \
           -mno-red-zone -O3 -Wall -Wextra -Iuserland -Ibuild
ULDFLAGS := -nostdlib -static -pie -m elf_x86_64 -z max-page-size=0x1000 \
            -T userland/init.ld

REQUIRED_TOOLS := nasm xorriso git python3
MISSING_TOOLS := $(strip $(foreach t,$(REQUIRED_TOOLS),$(if $(shell which $(t) 2>/dev/null),,$(t))))
ifneq ($(MISSING_TOOLS),)
  $(info ERROR: missing required host tools: $(MISSING_TOOLS))
  $(info Install with (Debian/Ubuntu):)
  $(info   sudo apt install -y build-essential nasm xorriso git python3 python3-pil librsvg2-bin qemu-system-x86 ovmf)
  $(error Aborting)
endif
ifeq ($(shell which rsvg-convert 2>/dev/null),)
  $(info ERROR: rsvg-convert (librsvg2-bin) not found.)
  $(info   Debian/Ubuntu: sudo apt install librsvg2-bin)
  $(error Aborting)
endif
PIL_OK := $(strip $(shell python3 -c "from PIL import Image" 2>/dev/null && echo OK))
ifneq ($(PIL_OK),OK)
  $(info ERROR: Python Pillow module not found.)
  $(info   pip3 install Pillow     -- or --     sudo apt install python3-pil)
  $(error Aborting)
endif

.PHONY: all iso run clean distclean limine assets portable-check
all: $(KERNEL) $(USER_ELF) $(WM_ELF) build/nyra.elf build/files.elf build/settings.elf build/editor.elf build/browser.elf build/media.elf build/camera.elf build/viewer.elf build/calc.elf build/sysinfo.elf build/apk.elf initrd_root/repo/calc.ypkg initrd_root/repo/sysinfo.ypkg

assets: $(KORA_BIN) $(KORA_H) $(WP_BIN) $(CUR_BIN) $(CUR_H)

$(KORA_BIN) $(KORA_H): scripts/gen_assets.py
	@mkdir -p build
	@chmod +x scripts/gen_assets.py 2>/dev/null || true
	python3 scripts/gen_assets.py
	@test -s $(KORA_BIN) || { echo "ERROR: kora.bin missing"; exit 1; }

$(WP_BIN): scripts/gen_wallpaper_pack.py
	@mkdir -p build $(WP_DIR)
	@chmod +x scripts/gen_wallpaper_pack.py 2>/dev/null || true
	python3 scripts/gen_wallpaper_pack.py
	@test -s $(WP_BIN) || { echo "ERROR: wallpaper.bin missing"; exit 1; }

$(WP_PNG): $(WP_BIN)
	@: default.png side-effect

$(CUR_BIN) $(CUR_H): scripts/gen_cursors.py $(wildcard kora/cursors/*.png) $(wildcard kora/cursors/*.hot)
	@mkdir -p build
	@chmod +x scripts/gen_cursors.py 2>/dev/null || true
	python3 scripts/gen_cursors.py
	@test -s $(CUR_BIN) || { echo "ERROR: cursors.bin missing"; exit 1; }

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

# Userland
build/start.o: userland/start.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/start.c -o build/start.o
build/init.o: userland/init.c userland/sys.h userland/gfx.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/init.c -o build/init.o
build/wm_main.o: userland/wm_main.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/wm_main.c -o build/wm_main.o
build/theme.o: userland/theme/theme.c userland/theme/theme.h userland/sys.h
	$(CC) $(UCFLAGS) -Iuserland/theme -c userland/theme/theme.c -o build/theme.o
build/wm.o: userland/wm.c userland/wm.h userland/sys.h userland/gfx.h userland/cursors.h userland/theme/theme.h userland/jpeg_enc.h $(KORA_H) $(CUR_H) assets
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/wm.c -o build/wm.o
build/wm_damage.o: userland/wm_damage.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_damage.c -o build/wm_damage.o
build/wm_windows.o: userland/wm_windows.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_windows.c -o build/wm_windows.o
build/wm_dock.o: userland/wm_dock.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_dock.c -o build/wm_dock.o
build/wm_panel.o: userland/wm_panel.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_panel.c -o build/wm_panel.o
build/wm_launcher.o: userland/wm_launcher.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_launcher.c -o build/wm_launcher.o
build/wm_overlays.o: userland/wm_overlays.c userland/wm.h
	$(CC) $(UCFLAGS) -c userland/wm_overlays.c -o build/wm_overlays.o
build/gfx.o: userland/gfx.c userland/gfx.h userland/sys.h $(KORA_H) assets
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/gfx.c -o build/gfx.o
build/wallpaper_code.o: userland/wallpaper.c userland/gfx.h userland/sys.h assets
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/wallpaper.c -o build/wallpaper_code.o
build/assets.o: $(KORA_BIN)
	@mkdir -p build
	cd build && $(LD) -r -b binary -o assets.o kora.bin
build/wallpaper.o: $(WP_BIN)
	@mkdir -p build
	cd build && $(LD) -r -b binary -o wallpaper.o wallpaper.bin
build/cursors.o: $(CUR_BIN)
	@mkdir -p build
	cd build && $(LD) -r -b binary -o cursors.o cursors.bin

# init = the SUPERVISOR + text console.  Small: no wm, no wallpaper, no
# cursors - just the text renderer (gfx) + icon atlas (assets, which gfx pulls
# in for icon_get).  The compositor is a separate /bin/wm binary.
$(USER_ELF): build/start.o build/init.o build/gfx.o build/assets.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/init.o build/gfx.o build/assets.o -o $@

# /bin/wm = the ring-3 compositor (the session init fork/execs).
$(WM_ELF): build/start.o build/wm_main.o build/wm.o build/wm_damage.o build/wm_windows.o build/wm_dock.o build/wm_panel.o build/wm_launcher.o build/wm_overlays.o build/cursors_lib.o build/gfx.o build/wallpaper_code.o build/theme.o build/assets.o build/wallpaper.o build/cursors.o build/jpeg_enc.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/wm_main.o build/wm.o build/wm_damage.o build/wm_windows.o build/wm_dock.o build/wm_panel.o build/wm_launcher.o build/wm_overlays.o build/cursors_lib.o build/gfx.o build/wallpaper_code.o build/theme.o build/assets.o build/wallpaper.o build/cursors.o build/jpeg_enc.o -o $@

# /bin/hello
build/hello.o: userland/hello.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/hello.c -o build/hello.o
build/hello.elf: build/start.o build/hello.o userland/init.ld
	@mkdir -p build
	$(LD) $(ULDFLAGS) build/start.o build/hello.o -o $@


# Simple bundled GUI apps
build/gui_apps_%.o: userland/gui_apps.c userland/sys.h userland/gfx.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -DAPP_NAME=\"$*\" $< -c -o $@

build/gui_files.o: userland/gui_apps.c userland/sys.h userland/gfx.h userland/cursors.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -DAPP_FILES -DAPP_NAME=\"Files\" -c $< -o $@
build/gui_settings.o: userland/gui_apps.c userland/sys.h userland/gfx.h userland/cursors.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -DAPP_SETTINGS -DAPP_NAME=\"Settings\" -c $< -o $@
build/gui_editor.o: userland/gui_apps.c userland/sys.h userland/gfx.h userland/cursors.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -DAPP_EDITOR -DAPP_NAME=\"Text\" -c $< -o $@
build/gui_browser.o: userland/gui_apps.c userland/sys.h userland/gfx.h userland/cursors.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -DAPP_BROWSER -DAPP_NAME=\"About\" -c $< -o $@

build/files.elf: build/start.o build/gui_files.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/gui_files.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o -o $@
build/settings.elf: build/start.o build/gui_settings.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/gui_settings.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o -o $@
build/editor.elf: build/start.o build/gui_editor.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/gui_editor.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o -o $@
build/browser.elf: build/start.o build/gui_browser.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/gui_browser.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o -o $@

initrd_root/bin/files: build/files.elf
	@mkdir -p initrd_root/bin
	cp build/files.elf initrd_root/bin/files; chmod 755 initrd_root/bin/files
initrd_root/bin/settings: build/settings.elf
	cp build/settings.elf initrd_root/bin/settings; chmod 755 initrd_root/bin/settings
initrd_root/bin/editor: build/editor.elf
	cp build/editor.elf initrd_root/bin/editor; chmod 755 initrd_root/bin/editor
initrd_root/bin/browser: build/browser.elf
	cp build/browser.elf initrd_root/bin/browser; chmod 755 initrd_root/bin/browser

# /bin/nyra - Nyra Terminal (unique shell replacing settings)
build/nyra.o: userland/nyra.c userland/sys.h userland/gfx.h userland/apk.h userland/fsutil.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/nyra.c -o build/nyra.o
build/cursors_lib.o: userland/cursors.c userland/cursors.h $(CUR_H)
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/cursors.c -o build/cursors_lib.o
build/nyra.elf: build/start.o build/nyra.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o build/apk_core.o build/fsutil.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/nyra.o build/cursors_lib.o build/gfx.o build/theme.o build/assets.o build/cursors.o build/apk_core.o build/fsutil.o -o $@

# /bin/media - Motion-JPEG video player
build/media.o: userland/media.c userland/sys.h userland/gfx.h userland/jpeg.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/media.c -o build/media.o
build/jpeg.o: userland/jpeg.c userland/jpeg.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/jpeg.c -o build/jpeg.o
build/jpeg_enc.o: userland/jpeg_enc.c userland/jpeg_enc.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/jpeg_enc.c -o build/jpeg_enc.o
build/clip.o: userland/clip.mjpeg
	@mkdir -p build
	cp userland/clip.mjpeg build/clip.mjpeg
	cd build && $(LD) -r -b binary -o clip.o clip.mjpeg
build/media.elf: build/start.o build/media.o build/jpeg.o build/clip.o build/gfx.o build/assets.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/media.o build/jpeg.o build/clip.o build/gfx.o build/assets.o -o $@

# ---- roadmap #5: camera + viewer ----
build/fsutil.o: userland/fsutil.c userland/fsutil.h userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/fsutil.c -o build/fsutil.o

build/camera.o: userland/camera.c userland/sys.h userland/gfx.h userland/jpeg_enc.h userland/fsutil.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/camera.c -o build/camera.o
build/camera.elf: build/start.o build/camera.o build/fsutil.o build/jpeg_enc.o build/gfx.o build/assets.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/camera.o build/fsutil.o build/jpeg_enc.o build/gfx.o build/assets.o -o $@

build/viewer.o: userland/viewer.c userland/sys.h userland/gfx.h userland/jpeg.h userland/fsutil.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/viewer.c -o build/viewer.o
build/viewer.elf: build/start.o build/viewer.o build/fsutil.o build/jpeg.o build/gfx.o build/assets.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/viewer.o build/fsutil.o build/jpeg.o build/gfx.o build/assets.o -o $@

# ---- roadmap #7: package manager + Calculator (installable) package ----
build/apk_core.o: userland/apk_core.c userland/apk.h userland/fsutil.h userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/apk_core.c -o build/apk_core.o
build/apk.o: userland/apk.c userland/apk.h userland/fsutil.h userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/apk.c -o build/apk.o
build/apk.elf: build/start.o build/apk.o build/apk_core.o build/fsutil.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/apk.o build/apk_core.o build/fsutil.o -o $@

build/calc.o: userland/calc.c userland/sys.h userland/gfx.h userland/fsutil.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/calc.c -o build/calc.o
build/calc.elf: build/start.o build/calc.o build/fsutil.o build/gfx.o build/assets.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/calc.o build/fsutil.o build/gfx.o build/assets.o -o $@

build/sysinfo.o: userland/sysinfo.c userland/sys.h
	@mkdir -p build
	$(CC) $(UCFLAGS) -c userland/sysinfo.c -o build/sysinfo.o
build/sysinfo.elf: build/start.o build/sysinfo.o userland/init.ld
	$(LD) $(ULDFLAGS) build/start.o build/sysinfo.o -o $@

# the Calculator ships ONLY as an installable package (demonstrates apk)
initrd_root/repo/calc.ypkg: build/calc.elf scripts/mkpkg.py
	@mkdir -p initrd_root/repo
	python3 scripts/mkpkg.py initrd_root/repo/calc.ypkg --name calc --version 1.0.0 \
	    --desc "A simple calculator" --icon calculator --desktop \
	    --file build/calc.elf:/bin/calc:exec

# a second package: a CLI system-info tool (proves the repo holds N packages)
initrd_root/repo/sysinfo.ypkg: build/sysinfo.elf scripts/mkpkg.py
	@mkdir -p initrd_root/repo
	python3 scripts/mkpkg.py initrd_root/repo/sysinfo.ypkg --name sysinfo --version 1.0.0 \
	    --desc "CLI system info" --icon terminal \
	    --file build/sysinfo.elf:/bin/sysinfo:exec

# seed the desktop-entry dir (apk + the compositor scan need it)
initrd_root/usr/share/applications/.keep:
	@mkdir -p initrd_root/usr/share/applications
	@touch $@

initrd_root/bin/init: $(USER_ELF)
	@mkdir -p initrd_root/bin
	cp $(USER_ELF) initrd_root/bin/init
	@chmod 755 initrd_root/bin/init

initrd_root/bin/wm: $(WM_ELF)
	@mkdir -p initrd_root/bin
	cp $(WM_ELF) initrd_root/bin/wm
	@chmod 755 initrd_root/bin/wm

initrd_root/bin/hello: build/hello.elf
	@mkdir -p initrd_root/bin
	cp build/hello.elf initrd_root/bin/hello
	@chmod 755 initrd_root/bin/hello

initrd_root/bin/nyra: build/nyra.elf
	@mkdir -p initrd_root/bin
	cp build/nyra.elf initrd_root/bin/nyra
	@chmod 755 initrd_root/bin/nyra

initrd_root/bin/media: build/media.elf
	@mkdir -p initrd_root/bin
	cp build/media.elf initrd_root/bin/media
	@chmod 755 initrd_root/bin/media

initrd_root/bin/camera: build/camera.elf
	@mkdir -p initrd_root/bin
	cp build/camera.elf initrd_root/bin/camera
	@chmod 755 initrd_root/bin/camera

initrd_root/bin/viewer: build/viewer.elf
	@mkdir -p initrd_root/bin
	cp build/viewer.elf initrd_root/bin/viewer
	@chmod 755 initrd_root/bin/viewer

initrd_root/bin/apk: build/apk.elf
	@mkdir -p initrd_root/bin
	cp build/apk.elf initrd_root/bin/apk
	@chmod 755 initrd_root/bin/apk

# The Linux-ABI test binaries + dynamic-linking demo are GENUINE Linux ELF
# objects, so they are built with the HOST toolchain — which must emit ELF
# (i.e. a Linux host; macOS clang emits Mach-O).  On non-ELF hosts they are
# skipped (the core YartOS build is unaffected).  See LINUX_DEMO_DEPS below.
HOST_IS_LINUX := $(shell uname -s | grep -qi linux && echo 1 || echo 0)

# A genuine Linux static binary (ET_EXEC, Linux syscall ABI) embedded for
# verifying the Linux-ABI layer.
initrd_root/bin/test_linux: tests/test_linux.S
	@mkdir -p initrd_root/bin
	gcc -nostdlib -static -no-pie -o $@ tests/test_linux.S
	@chmod 755 $@

# threads (clone+futex) + sockets + execve test, and its execve target
initrd_root/bin/test_linux2: tests/test_linux2.S
	@mkdir -p initrd_root/bin
	gcc -nostdlib -static -no-pie -o $@ tests/test_linux2.S
	@chmod 755 $@

initrd_root/bin/test_echo: tests/test_echo.S
	@mkdir -p initrd_root/bin
	gcc -nostdlib -static -no-pie -o $@ tests/test_echo.S
	@chmod 755 $@

initrd_root/bin/test_tls: tests/test_tls.S
	@mkdir -p initrd_root/bin
	gcc -nostdlib -static -no-pie -o $@ tests/test_tls.S
	@chmod 755 $@

# DYNAMIC LINKING demo: a real dynamic linker (interpreter) + shared lib +
# a dynamically-linked program.  All are genuine Linux ELF objects.
initrd_root/lib/ld-yart.so: tests/ld-yart.c
	@mkdir -p initrd_root/lib
	gcc -shared -fPIC -nostdlib -Wl,-e,_start -o $@ tests/ld-yart.c

initrd_root/lib/libgreet.so: tests/libgreet.c
	@mkdir -p initrd_root/lib
	gcc -shared -fPIC -nostdlib -o $@ tests/libgreet.c

initrd_root/bin/dynhello: tests/dynhello.c initrd_root/lib/ld-yart.so initrd_root/lib/libgreet.so
	@mkdir -p initrd_root/bin
	gcc -nostdlib -pie -fPIC -Wl,--dynamic-linker=/lib/ld-yart.so \
	    -o $@ tests/dynhello.c -L initrd_root/lib -lgreet
	@chmod 755 $@

# TLS demo: a dynamically-linked program + a .so, both using __thread
# variables (exercises __tls_get_addr + DTPMOD64/DTPOFF64 + static TLS).
initrd_root/lib/libtls.so: tests/libtls.c
	@mkdir -p initrd_root/lib
	gcc -shared -fPIC -nostdlib -o $@ tests/libtls.c

initrd_root/bin/tlsdemo: tests/tlsdemo.c initrd_root/lib/ld-yart.so initrd_root/lib/libtls.so
	@mkdir -p initrd_root/bin
	gcc -nostdlib -pie -fPIC -Wl,--dynamic-linker=/lib/ld-yart.so \
	    -o $@ tests/tlsdemo.c -L initrd_root/lib -ltls
	@chmod 755 $@

# IFUNC demo: a .so exporting an STT_GNU_IFUNC symbol (the resolver path).
initrd_root/lib/libifunc.so: tests/libifunc.c
	@mkdir -p initrd_root/lib
	gcc -shared -fPIC -nostdlib -o $@ tests/libifunc.c

initrd_root/bin/ifuncdemo: tests/ifuncdemo.c initrd_root/lib/ld-yart.so initrd_root/lib/libifunc.so
	@mkdir -p initrd_root/bin
	gcc -nostdlib -pie -fPIC -Wl,--dynamic-linker=/lib/ld-yart.so \
	    -o $@ tests/ifuncdemo.c -L initrd_root/lib -lifunc
	@chmod 755 $@

# COPY-relocation demo (a .so referencing a global defined in the program).
initrd_root/lib/libcopy.so: tests/libcopy.c
	@mkdir -p initrd_root/lib
	gcc -shared -fPIC -nostdlib -o $@ tests/libcopy.c

initrd_root/bin/copydemo: tests/copydemo.c initrd_root/lib/ld-yart.so initrd_root/lib/libcopy.so
	@mkdir -p initrd_root/bin
	gcc -nostdlib -pie -fPIC -Wl,--dynamic-linker=/lib/ld-yart.so \
	    -o $@ tests/copydemo.c -L initrd_root/lib -lcopy
	@chmod 755 $@

# Only build/embed the Linux demos on an ELF-capable (Linux) host.
ifeq ($(HOST_IS_LINUX),1)
LINUX_DEMO_DEPS := initrd_root/bin/test_linux initrd_root/bin/test_linux2 \
                   initrd_root/bin/test_echo initrd_root/bin/test_tls \
                   initrd_root/bin/dynhello initrd_root/bin/tlsdemo \
                   initrd_root/bin/ifuncdemo initrd_root/bin/copydemo \
                   initrd_root/lib/ld-yart.so initrd_root/lib/libgreet.so \
                   initrd_root/lib/libtls.so initrd_root/lib/libifunc.so \
                   initrd_root/lib/libcopy.so
else
LINUX_DEMO_DEPS :=
$(info NOTE: non-ELF host ($(shell uname -s)) — skipping Linux-ABI demo binaries.)
endif

initrd_root/etc/motd:
	@mkdir -p initrd_root/etc
	@printf "Yart OS MAX - calm, fast, wifi, 10MiB FS, Nyra Terminal\n" > $@

initrd_root/etc/yart.conf:
	@mkdir -p initrd_root/etc
	@printf "hostname=yart\ntheme=quartz\naccent=#5BA7DF\nborder=#5BA7DF\ncorner_radius=8\ndock.position=bottom\ndock.icon_size=34\ndock.spacing=44\nwallpaper.mode=image\nwallpaper.path=/YartOS/kora/wallpaper.bmp\nwallpaper.index=0\nfont.system=default\nfont.terminal=default\ndisplay.fps=60\ntime.format24=1\ncursor.size=24\nwifi.enabled=1\nfs.max=1\n" > $@

initrd_root/home/yart/cursor.conf:
	@mkdir -p initrd_root/home/yart
	@printf "theme=photo-white\n" > $@

$(LIMINE):
	@chmod +x scripts/get-limine.sh 2>/dev/null || true
	bash ./scripts/get-limine.sh

build/initrd.tar: initrd_root/etc/motd initrd_root/etc/yart.conf initrd_root/home/yart/cursor.conf $(BMP_WALL) initrd_root/bin/init initrd_root/bin/wm initrd_root/bin/hello initrd_root/bin/nyra initrd_root/bin/files initrd_root/bin/settings initrd_root/bin/editor initrd_root/bin/browser initrd_root/bin/media initrd_root/bin/camera initrd_root/bin/viewer $(LINUX_DEMO_DEPS) initrd_root/bin/apk initrd_root/repo/calc.ypkg initrd_root/repo/sysinfo.ypkg initrd_root/usr/share/applications/.keep
	@mkdir -p build
	cd initrd_root && tar --format=ustar -cf ../build/initrd.tar .

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
	  echo "ERROR: qemu-system-x86_64 not found"; exit 1; fi
	@chmod +x scripts/run-qemu.sh 2>/dev/null || true
	bash ./scripts/run-qemu.sh

run-bios: $(ISO)
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 1024 \
	    -cdrom $(ISO) -boot d -serial stdio

clean:
	rm -rf build $(ISO_ROOT) $(ISO) \
	           initrd_root/bin initrd_root/repo initrd_root/usr \
	           initrd_root/lib/ld-yart.so initrd_root/lib/libgreet.so \
	           $(BMP_WALL) initrd_root/etc/motd initrd_root/etc/yart.conf \
	           initrd_root/home/yart/cursor.conf initrd_root/YartOS \
	           yart-disk.img runlogs audio-out.wav
	@for f in *.ppm *.png; do \
	  if [ -e "$$f" ] && ! git ls-files --error-unmatch "$$f" >/dev/null 2>&1; then rm -f "$$f"; fi; \
	done
distclean: clean
	rm -rf $(LIMINE)

# ---- portability check: fail if the tree carries any non-source cruft ----
portable-check:
	@ok=1; \
	for f in yart.iso yart-disk.img; do \
	  if [ -e "$$f" ]; then echo "NOT PORTABLE: $$f present (run 'make clean')"; ok=0; fi; \
	done; \
	for f in *.ppm *.png; do \
	  if [ -e "$$f" ] && ! git ls-files --error-unmatch "$$f" >/dev/null 2>&1; then \
	    echo "NOT PORTABLE: untracked $$f present (run 'make clean')"; ok=0; fi; \
	done; \
	for d in initrd_root/bin initrd_root/repo initrd_root/usr; do \
	  if [ -e "$$d" ]; then echo "NOT PORTABLE: $$d present (run 'make clean')"; ok=0; fi; \
	done; \
	for f in initrd_root/lib/ld-yart.so initrd_root/lib/libgreet.so; do \
	  if [ -e "$$f" ]; then echo "NOT PORTABLE: $$f present (run 'make clean')"; ok=0; fi; \
	done; \
	if [ -n "$$(ls runlogs/ 2>/dev/null)" ]; then echo "NOT PORTABLE: runlogs/ not empty"; ok=0; fi; \
	if [ "$$ok" = 1 ]; then echo "portable: tree is clean"; else exit 1; fi

-include $(DEPS)
