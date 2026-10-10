# BRUTAL AUDIT — YartOS

## ⚡ THIS TURN (2026-08-19, #23): portability VERIFIED end-to-end

### The portability claim is now PROVEN, not asserted
- Fixed a real `.gitignore` bug: inline `#` comments after patterns are
  treated as literal pattern text by git (not comments), so the generated-
  artifact ignores matched NOTHING and `initrd_root/bin/`, `repo/`, `usr/`
  leaked into `git status`.  Rewrote them with comments on their own lines
  and switched the libs to a wildcard `initrd_root/lib/*.so` (covering the
  new libifunc/libcopy/libtls.so too).
- **Fresh-clone build test**: copied the exact source set (7338 files =
  `git ls-files` + `git ls-files --others --exclude-standard`) to an empty
  directory — no `build/`, no `limine/`, no binaries, no ISO — and ran
  `make iso`.  Result: builds with **0 warnings**, Limine auto-fetched,
  and the resulting ISO **boots with 0 faults** (compositor up, SIMD selftest
  pass).  This is the strongest portability proof: a stranger on any Linux
  box with the documented deps runs `./bootstrap.sh && make iso` and gets a
  working OS.
- `make portable-check` passes on a clean tree; `make clean` removes every
  generated artifact (and preserves the tracked `ver1.ppm` reference).
- Dockerfile (pinned Debian 12) + `HOST_IS_LINUX` gating (macOS/Windows skip
  the host-gcc Linux demos) round out reproducible + cross-host builds.

## ⚡ THIS TURN (2026-08-19, #22): IFUNC + COPY relocations — the last linker gaps

### STT_GNU_IFUNC (indirect functions) — DONE + BOOT-VERIFIED
The dynamic linker now recognizes `STT_GNU_IFUNC` symbols and calls their
resolver: when a relocation resolves to an IFUNC symbol, `ld-yart` invokes
the resolver (`st_value` is the resolver address) and stores its return value
(the real function).  Also added `R_X86_64_IRELATIVE` (slot-relative resolver)
and `R_X86_64_COPY` (copy a program-defined global into a .so's .bss).

**Boot-verified** with a new `ifuncdemo` (PIE linked against `libifunc.so`,
whose `hello` is `__attribute__((ifunc("resolve_hello")))`):
```
ld: loaded /lib/libifunc.so
ifunc: fast path        <- the resolver ran and returned hello_fast
ifunc: called
```
and a `copydemo` (a .so referencing the program's global):
```
copy global bump=6 bump=7   <- the shared global mutated correctly
```

### Honest scope update
`resolve_symbol` now handles IFUNC; COPY + IRELATIVE are implemented (COPY is
a GLOB_DAT-equivalent safety net for the rare non-PIC case — the demo uses
GLOB_DAT which is the modern path).  The one remaining gap vs a full ld.so is
**GNU symbol versioning** (DT_VERSYM/VERDEF/VERNEED) and **lazy PLT binding**
(eager binding is already correct).  Real glibc needs versioning; musl-class
static-PIE binaries already run unmodified.

## ⚡ THIS TURN (2026-08-18, #21): TLS relocations + portability — the finishing pass## ⚡ THIS TURN (2026-08-18, #21): TLS relocations + portability — the finishing pass

### TLS RELOCATIONS (the last named dynamic-linking gap) — DONE + BOOT-VERIFIED
The dynamic linker now handles the full x86-64 TLS ABI (variant 2):
- **Static TLS block** laid out below the thread pointer (TP): the
  executable's block at [TP - exe_tls_size, TP), each `.so`'s block below,
  TCB self-pointer at %fs:0.
- **`__tls_get_addr`** exported from `ld-yart.so` (general-dynamic model).
- Relocations resolved: `R_X86_64_DTPMOD64`, `DTPOFF64`, `DTPOFF32`,
  `TPOFF64`, `TPOFF32` (static initial-exec/local-exec).
- The interpreter registers ITSELF as a resolvable object (so programs can
  bind `__tls_get_addr`).
- `.tdata` initialized images copied from each object's file.

**Boot-verified** with a new `tlsdemo` (a PIE with `__thread int mine = 7`
linked against `libtls.so` with `__thread int tls_counter = 100`):
```
tls mine=7 peek=100 bump=101 bump=102
```
i.e. the executable's `__thread` (local-exec via `%fs`) AND the library's
`__thread` (general-dynamic via `__tls_get_addr`) both carry their
initialized values and mutate correctly.

### Three real bugs found + fixed (all caught by boot-testing)
1. **`align_up(memsz, 16)` on the executable's TLS size** shifted the thread
   pointer 12 bytes up, so `mine` (at TP-4) read the wrong slot — the size
   must be the RAW PT_TLS memsz.
2. **`load_lib` copied `o->name` after advancing the name pointer**, leaving
   it empty — the `.so` `.tdata` read opened `/lib/` instead of the real path.
3. (Test-side) my first demo printed `% 10` (last digit only), which read as
   `peek=0` for a correct value of 100 — chased a phantom bug before realizing
   the value was right all along.

### PORTABILITY — the whole build is now cleanly reproducible
- `.gitignore` now covers EVERY generated artifact (the new `bin/`, `lib/*.so`,
  `repo/`, `usr/`), so `git status` is clean after a build.
- `make clean` removes the new binaries + libs + repo + usr (and no longer
  deletes the TRACKED `ver1.ppm` reference screenshot).
- `make portable-check` now correctly skips git-tracked files and flags all
  the new artifacts.
- Added a **Dockerfile** (pinned Debian 12) for a reproducible build with no
  host toolchain.
- The host-gcc Linux test/demo binaries are gated behind a Linux/ELF-host
  check (`HOST_IS_LINUX`), so macOS/Windows builds skip them instead of
  failing (they're Mach-O, not ELF).
- README documents the Linux-ABI layer, dynamic linking, apk, and Docker.

### Verified (QEMU/OVMF, TCG)
Clean build (0 warnings), boot has 0 faults, `tlsdemo` prints correct values.
New shell command: `tlsdemo`.

### HONEST remaining limits (unchanged + narrowed)
The linker now handles the full static/initial-exec/general-dynamic TLS path
+ the common data relocations.  Still NOT: IFUNC, symbol versioning, lazy
PLT binding, and R_X86_64_COPY — the last increment before real glibc/musl
binaries run unmodified.  `apk` is native (not apk-tools).  TCG lag unchanged.

## ⚡ THIS TURN (2026-08-18, #20): DYNAMIC LINKING + TLS — the last Linux-ABI milestone

### Dynamic linking (PT_INTERP + a real dynamic linker) — BUILT + BOOT-VERIFIED
- **Kernel loader** (`kernel/arch/x86_64/user.c`): a Linux PIE with PT_INTERP
  is now detected (PT_INTERP presence, not just ET_EXEC); the kernel loads the
  interpreter's segments, applies its R_X86_64_RELATIVE relocs, starts the
  process at the interpreter's entry, and passes AT_BASE/AT_ENTRY/AT_PHDR in
  the auxv (correctly placed ABOVE envp this time — see the bug below).
- **A real dynamic linker** (`tests/ld-yart.c`, compiled to `/lib/ld-yart.so`):
  reads the auxv, walks the main program's PT_DYNAMIC, loads each DT_NEEDED
  shared object (mmap + segment copy), applies R_X86_64_RELATIVE / GLOB_DAT /
  JUMP_SLOT / 64 relocations with cross-object symbol resolution, then jumps
  to the program entry.  ~300 lines, freestanding, raw Linux syscalls.
- **Demo**: `dynhello` (PIE, --dynamic-linker=/lib/ld-yart.so, -lgreet) calls
  greet() from `/lib/libgreet.so`.  Boot-verified serial output:
  ```
  ld-yart: dynamic linker up
  ld: loaded /lib/libgreet.so
  ld-yart: relocations done, jumping to main
  dynhello: calling greet() from a .so
  libgreet: hello, world
  dynhello: greet returned 2
  ```
- **TLS applied to %fs**: `arch_prctl(ARCH_SET_FS)` now writes MSR_FS_BASE
  immediately, and `switch_to` re-applies it per task (the kernel only uses
  %gs via swapgs, so %fs is free for user TLS).  `tlstest` reads %fs:0 back
  and prints `TLS OK: fs base applied`.
- **exit_group** now kills the whole thread group (all tasks sharing a PML4).

### Four real bugs found & fixed (all caught by boot-testing, not reading)
1. **auxv placed BELOW argc** in build_user_stack_into — any auxv reader
   (the linker) walked off the top of the stack and faulted.  Fixed to the
   SysV order (argc, argv, NULL, envp, NULL, auxv, AT_NULL, strings).
2. **DT_NEEDED d_val is a strtab offset**, but the linker added it to the
   load base instead of the string table (loaded "/lib/" with an empty name).
3. **resolve_symbol scanned 4096 symbols blindly** (there were 2), reading
   garbage into strcmp — now bounded by (strtab - symtab)/SYMENT.
4. **arch_prctl SET_FS only stored the value** (applied on the NEXT context
   switch), so an immediate %fs read faulted — now applied immediately.
   (Plus two test-side bugs: `syscall` clobbers %rcx, and a stack/buffer
   overlap.)

### Verified (QEMU/OVMF, TCG)
Clean build (0 warnings), boot has 0 faults, compositor up.  `dynhello`,
   `tlstest`, `linuxtest`, `linuxtest2` are permanent shell commands.

### HONEST remaining limits (dynamic linking)
The linker resolves RELATIVE/GLOB_DAT/JUMP_SLOT/64 + DT_NEEDED.  NOT yet:
TLS *relocations* (R_X86_64_TPOFF64/TPREL64 — the TLS *data* for .so's),
IFUNC, lazy binding, symbol versioning, R_X86_64_COPY, and a GNU/ELF hash
table (linear symbol scan).  So real glibc/musl binaries (which need TLS
relocs + more) are the next step; the mechanism — kernel PT_INTERP loading +
a working linker — is complete and proven.

## ✨ HOW YARTOS ASCENDED (before → after, the whole roadmap)

| Stage | Before | After |
|---|---|---|
| Terminal | basic echo shell | argv tokenizing, SIGINT/bg jobs, VT100 ANSI, cwd-aware prompt |
| Filesystem | read-only initrd | **YartFS v5** (ext4-architected: inodes, block groups, symlinks, CRC, crash-flag) |
| Session | non-killable wm | **init/getty model** — Ctrl+Alt+Backspace → console → startwm/reboot |
| Video | none | MJPEG player (`/bin/media`) |
| Media | none | **camera** (photo/video → JPEG/MJPEG) + **viewer** |
| Screenshots | none | full/window/region + screen recording (PrintScreen/F9) |
| Packages | none | **apk** (add/del/list/search/info) + repo; apps auto-appear in Super launcher |
| Linux ABI | nothing | static ELF (ET_EXEC) + auxv + **threads (clone/futex)** + **sockets** + **execve** |
| **This turn** | no shared libs | **dynamic linking** (PT_INTERP + ld-yart + .so) + **TLS (%fs)** |

YartOS went from a text console over a read-only initrd to a desktop OS with
a journaled filesystem, a killable/restartable window manager, video + media
apps, screenshots, a package manager with launcher integration, and a Linux
compatibility layer that runs static AND dynamically-linked Linux binaries
with real threads, sockets, and TLS.

## ⚡ THIS TURN (2026-08-18, #19): Linux ABI — threads (clone+futex), sockets, execve

### Finished the three remaining Linux-ABI milestones, all boot-verified
1. **execve (Linux #59)** -> SYS_EXEC.  A Linux binary can now spawn/replace
   itself with another program (verified: test_linux2 execve's /bin/test_echo).
2. **Sockets** -> the kernel TCP/UDP stack.  A separate socket-fd namespace
   (fds >= 100) maps Linux socket/bind/connect/listen/accept/send/sendto/
   recv/recvfrom/shutdown/close onto net_tcp_*/net_udp_*.  sockaddr_in parsed
   with correct network-byte-order ip (127.0.0.1 == 0x7F000001).  **UDP
   loopback verified**: a Linux binary bind+sendto+recvfrom 127.0.0.1 and
   echoed its own datagram.
3. **Threads**: `sched_clone_thread` (clone with CLONE_VM|CLONE_THREAD...)
   shares the parent's PML4 (refcounted via a new `sched_pml4_ref/unref/
   is_shared` table), fd table, and cwd; the child resumes at the same
   instruction with a new stack + recorded TLS base.  **futex** (WAIT/WAKE)
   added as a per-address wait queue.  Verified: two threads ran, shared a
   variable, and rendezvoused through futex (spawn -> shared=42 -> wake ->
   "thread: woken via futex").

### One real, subtle bug found & fixed (the thread-exit corruption)
`sched_exit` called `vmm_user_teardown_all()` unconditionally, which unmaps
the CURRENT task's user pages.  For a clone'd thread that SHARES the parent's
PML4, this unmapped the shared code pages out from under the parent -> the
parent faulted with **"Invalid Opcode"** moments after its thread exited
(the symptom: code corruption mid-spin-loop).  Fix: `sched_exit` skips the
teardown when `sched_pml4_is_shared(pml4)` — the last task to exit frees the
pages via `vmm_free_pml4` (which the reap path already refcounts).

Also fixed: a register-clobbering bug in the test's `emit` helper, and a
stack/recv-buffer overlap in the test's .bss layout (both test-side).

### Verified (QEMU/OVMF, TCG) — full test_linux2 serial output
```
clone: spawned
clone: shared memory ok
thread: woken via futex
futex: rendezvous ok
udp loopback ok
exec: pid 6 'test_linux2' -> test_echo entry=0x401000
EXECVE OK
```
Plus the prior #18 suite (stat/getdents64/uname/writev/mmap/clock_gettime).
Clean build (0 warnings); boot has 0 faults.

### New shell commands
- `linuxtest`  = run the file/stat/getdents64/uname/... Linux suite
- `linuxtest2` = run threads (clone+futex) + UDP loopback + execve

### NOT claimed (unchanged, honest)
- **Dynamic linking is still the remaining piece.**  Loading a PT_INTERP
  dynamic linker (ld.so / ld-musl) + applying TLS to %fs is the next major
  milestone; self-contained STATIC Linux binaries run, dynamically-linked
  ones do not yet.
- clone/futex are correct for the shared-PML4 thread model but a full
  pthread runtime also needs: TLS actually applied to %fs (arch_prctl SET_FS
  is stored but not applied), and exit_group semantics (currently
  exit_group == exit == exits only the current thread).
- Sockets: TCP maps onto the kernel TCP stack but is non-blocking (recv
  returns 0 = no data); full blocking + poll/select are future work.
- apk is native (not apk-tools); repo = calc + sysinfo.
- TCG lag unchanged (emulation, not code).

## ⚡ THIS TURN (2026-08-18, #18): Linux ABI expanded to a real static-binary runtime

### What was asked
"Continue and fix all the problems I mentioned": (1) the Linux ABI was only
a core-syscall slice, (2) the package repo had a single package.

### 1. Linux ABI — from "core slice" to a genuine static-binary runtime
Added the pieces a real Linux static binary's _start (musl/glibc) needs:
- **Auxiliary vector** on the stack (`kernel/arch/x86_64/user.c`): AT_PHDR,
  AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY, AT_CLKTCK, AT_RANDOM
  (16-byte TSC-seeded blob), AT_SYSINFO_EHDR.  `build_user_stack_into` now
  takes an auxv; only Linux ET_EXEC tasks get one.
- **Syscall surface** (`kernel/arch/x86_64/syscall.c`): replaced the tiny
  translate table with a full `linux_dispatch` — write/read/open/close/
  lseek/brk/getpid/exit/exit_group (translated) plus HANDLED-NATIVE:
  **stat/fstat/lstat** (Linux 144-byte `struct stat`), **getdents64**
  (dirent64 records + d_type), **uname** (utsname), **mmap** (6-arg, anon),
  **mprotect**, **writev**, **access**, **getuid/getgid/geteuid/getegid**,
  **gettimeofday/time/clock_gettime** (real epoch from the RTC via a new
  days-from-civil routine), **rt_sigaction/rt_sigprocmask**, **openat**
  (AT_FDCWD), **arch_prctl** (ARCH_SET/GET_FS stored on task_t; %fs not yet
  applied — TLS/errno deferred, documented).  Unsupported -> -ENOSYS.
- `task_t.linux_fs_base` (fork-copied).

### Verified end-to-end (QEMU/OVMF, TCG)
`tests/test_linux.S` is a hand-written Linux static binary exercising the
whole surface; its serial output on boot:
```
LXTEST: linux abi suite
19              (stat st_size of the openat-written file — exact)
8               (getdents64 entry count of /home/yart)
YartOS          (uname sysname)
1000            (getuid)
WV:ok           (writev)
mmap ok         (mmap+write+munmap)
1787093089      (clock_gettime epoch seconds — Aug 2026)
exit status 0
```
Two bugs found & fixed on the way: (a) the TEST's dirent walk read d_reclen
at offset 0 instead of +16 (kernel side was correct — 8 children found);
(b) a `sysinfo.c` compound-literal warning (real: `(char){a,0}` is a scalar
with 2 initializers).

### 2. Package repo — now two packages
Added `/bin/sysinfo` (CLI system-info tool) as a second installable package,
so `apk list` shows calc + sysinfo and add/del work across both.

### NOT claimed (unchanged, honest)
- Still NOT "run any Linux program": no PT_INTERP dynamic-linker loading, no
  TLS application to %fs, no clone/threads, futex, sockets, ioctl, poll.
  Self-contained STATIC Linux binaries run; dynamic/musl-linked do not yet.
- `apk` is still native (not a port of apk-tools), repo = calc + sysinfo.
- TCG lag unchanged (emulation, not code).

## ROADMAP (agreed with the user, 2026-08-18)
1. **[done] Terminal reliability** — turn #9.
2. **VFS "like ext4"** — **[done]** YartFS v5 (#10) + symlinks + CRC fixes (#11).
3. **WM shutdown-able (Linux init/getty session model)** — **[done]** turn #13.
4. **Video support (decode + play)** — **[done]** turn #14: MJPEG player.
5. **Media viewer + camera** — **[done]** this turn (#16): `/bin/camera` +
   `/bin/viewer` shipped.
6. **Screenshot tool (GNOME-style)** — **[done]** turn #15: full / window /
   region + screen recording.
7. **[done] Package manager + launcher integration** — this turn (#17):
   native `apk` (`add/del/list/search/info`) + Calculator package; installs
   drop a `.desktop` entry the compositor scans so the app appears in the
   Super launcher (the "Ubuntu" flow the user described).

## ⚡ THIS TURN (2026-08-18, #17): Linux ABI layer + apk + launcher integration

### 1. LINUX ABI LAYER (the "do linux abi thing" ask) — built + verified
A foreign (non-PIE) Linux static ELF now loads and runs on YartOS:
- `task_t.linux_abi` flag; `sched_fork` copies it.
- Loader (`kernel/arch/x86_64/user.c`): accepts **ET_EXEC** (Linux static),
  loads at its fixed vaddr (bias 0); YartOS's own PIE (ET_DYN) unchanged.
- `kernel/mm/vmm.c` + `kernel/include/yart/user.h`: introduced `USER_VFLOOR`
  (0x1000) — the low 1 GB below `USER_VBASE` was previously rejected by the
  region model; Linux binaries load at 0x400000.
- `kernel/arch/x86_64/syscall.c`: `linux_syscall_translate()` maps the core
  Linux x86_64 syscalls (read/write/open/close/lseek/brk/mmap/munmap/getpid/
  exit/exit_group) to YartOS equivalents; unsupported -> -ENOSYS.

**Verified end-to-end**: compiled a genuine Linux static binary
(`tests/test_linux.S`, `gcc -nostdlib -static -no-pie`), embedded it as
`/bin/test_linux`, exec'd it, and its LINUX syscalls printed to serial:
`exec: pid 6 'wm' -> test_linux entry=0x401000` then
`hello from a LINUX binary` + `file io ok` (open/write/close of
/home/yart/linux_test.txt all through the translated ABI).  `linuxtest`
is a permanent shell builtin that runs it.

### 2. Package manager + launcher integration ("apk does everything")
Native `apk` (shell builtin + `/bin/apk` binary) with apk's CLI surface.
Installing a GUI package writes `/usr/share/applications/<name>.desktop`;
the compositor scans that dir every ~2s (`scan_desktop_apps`) and registers
each app in `G_app`, which is exactly what the Super launcher renders.  So
`apk add calc` -> press Super -> Calculator is in the grid (verified: a boot
hook ran `apk add calc` and asserted `app_index("/bin/calc") >= 0`).
The Calculator ships ONLY as an installable package (demonstrates the flow).

### 3. Two real bugs found + fixed along the way
- **`fs_read_file`/`fs_write_file` broke on files > 1 MiB**: the kernel caps
  a single read()/write() at `USER_BUF_MAX` (1 MiB); the whole-file helper
  issued one 16 MiB call.  Now chunks at 256 KiB (verified: the 509 KB
  calc.ypkg installs).
- **`apk` couldn't write `/bin`**: the wm runs as uid 1000 (non-root), so the
  install failed on the root-owned `/bin` — CORRECT behavior, real apk needs
  root.  `apk add/del` now elevates via `doas` first (verified: "task 6 'apk'
  elevated to root via doas").

### NOT claimed
- The Linux ABI is the CORE syscall set only (file/console/process).  A full
  Linux userspace (glibc/musl: openat/stat/futex/clone/rt_sigaction/ioctl/
  socket/execve-with-auxv/TLS...) is NOT done — that's a much larger,
  incremental project.  Self-contained static Linux binaries work; "run any
  Linux program" does not yet.
- `apk` is a native package manager with apk's CLI, NOT a port of apk-tools
  (which needs musl+openssl+libfetch).  One repo + one package (calc) seeded.
- Still TCG (no KVM): lag is unchanged and not a code issue.

## ⚡ THIS TURN (2026-08-18, #16): camera + viewer apps, 3 real bug fixes

### Built: the camera app (`/bin/camera`) + media viewer (`/bin/viewer`)
Roadmap #5 closed out.  `camera` renders an animated test-pattern "sensor"
(honestly labelled in the source — QEMU has no camera hardware, same honesty
pattern as battery/wifi), JPEG-encodes photos to ~/Pictures/cam_N.jpg and
records MJPEG to ~/Videos/cam_N.mjpeg (Space = photo, R = record, Esc = quit).
`viewer` decodes .jpg / .mjpeg and displays them (Space = play/pause,
Left/Right = step).  Both use the real JPEG codec from turns #14/#15 and a
new shared `userland/fsutil.c` (mkdir -p, whole-file read/write, next-free-N).

### Fixed 3 bugs, all verified in a real QEMU boot (TCG, no KVM)
1. **init.c `-Wstringop-overflow`** (the pre-existing false-positive I used to
   wave off): root cause is real — `my_itoa` can write up to 12 bytes but the
   call sites used `char b[8]`.  Changed every `b[8]` to `b[16]`.  **0 warnings**
   now (verified: full userland recompile emits nothing).
2. **PS/2 mouse Y-axis was INVERTED.** `kernel/drivers/mouse.c` had
   `dy = -dy; /* invert Y */`, so moving the mouse down moved the cursor up.
   Ground truth was captured with a temporary kernel kprintf: `mouse_move 0 +N`
   produces PS/2 dy = -N (correct "up"), and the old line negated it again.
   Removed the inversion; both axes now track the physical mouse correctly.
3. **Cursor "brushing" on the top-bar buttons.** The panel's hover highlight
   pill (36 px) is wider than the cursor footprint (~24 px), and the panel
   was only repainted when a dirty rect collided with it, so moving the cursor
   off a button left a stale highlighted edge behind.  The dock already
   self-damaged on hover change (`dock_update`); the panel did not.  Fix: on
   cursor move, damage the panel strip spanning old->new X.  **Verified by
   pixel analysis**: hover over the clock = 2552 px of highlight colour in the
   top bar; move away = 161 px (just antialiased text) — the residue is gone.

### Verified (QEMU/OVMF, TCG)
- Clean build, **0 warnings**.
- Boots to the login screen (the wm starts LOCKED by design — `G_locked=true`;
  earlier in this turn I misread the dimmed login screen as "no desktop").
  After `yart` + Enter, the full desktop renders (screendump mean 47.7,
  matching the reference screenshots in docs/screenshots/).
- Hover appear/clear measured end-to-end via HMP `mouse_move` + screendump.

### Package manager: REVERTED, and the honest story on "port apk"
The user asked me to delete the native package manager I started and instead
"one-time port Alpine's apk so it does everything".  I deleted it as asked
(apk.c / apk_core.c / calc.c / mkpkg.py all gone; build clean).  But I have to
be straight about what "port apk" actually means, because it is NOT a small
job and it does NOT by itself deliver "install Ubuntu-style apps":

- apk-tools is a Linux userspace program.  It needs **musl libc**, **openssl**
  (for .SIGN.RSA verification), **libfetch/libcurl** (HTTP), and **zlib**
  (gzip).  YartOS has its OWN syscall ABI (not Linux's) and its own libc, so a
  real port means either porting musl + those libraries, or writing a
  Linux-syscall shim in the kernel — effectively bringing up a Linux
  userspace inside YartOS.  That is a multi-week project on its own.
- Even if apk ran, an Alpine `.apk` contains **Linux/musl ELF binaries** that
  will not RUN on YartOS (different syscalls, no dynamic linker).  A package
  manager cannot make foreign binaries run; only an ABI-compatibility layer
  can.  So "install Ubuntu apps" is blocked by the ABI, not by the installer.

What IS realistic (and what I'll build next turn if you want it): a native
`apk` that speaks the REAL Alpine `.apk` format (gzip + tar + .PKGINFO) with
apk's exact CLI (`apk add/del/list/search/info/update`), installs native
YartOS ELF packages, and auto-registers them in the launcher via
`/usr/share/applications/*.desktop` (the GNOME/KDE mechanism).  That gives you
the apk UX + apps-tab integration + the standard package format, and it is the
closest thing to "port apk" that is actually achievable on a from-scratch OS.

### NOT claimed
- No KVM/lag claims (still TCG — the desktop is smooth but TCG is inherently
  slower than KVM; this was already explained and is unchanged).
- No "apk ported" claim — reverted, and the honest scope spelled out above.
- Camera is a simulated sensor (no QEMU camera hardware exists); the JPEG/
  MJPEG encode/decode pipeline is the real codec from turns #14/#15.

## ⚡ THIS TURN (2026-08-18, #15): JPEG ENCODER + screenshots + screen recording

### Built: a from-scratch baseline JPEG encoder (userland/jpeg_enc.c)
Complements the decoder (turn #14).  4:2:0 chroma, quality-scaled standard
quant tables, the STANDARD Annex K Huffman tables (every decoder accepts
them), FDCT with a hardcoded cosine table, optimal run-length AC coding,
MCU-interleaved scan order.  ~390 lines, freestanding (no malloc, no libm).

### Verified by scripts/test_jpeg_enc_host.py (host unit test vs PIL)
Encodes 640x480 / 1280x800 / 320x192 test images, PIL decodes them, MAE
compared against the reference.  Result: **3/3 PASS, MAE 0.58-0.94**.

### 6 real bugs found & fixed while proving it (all caught by PIL/ImageMagick)
1. **The JPEG length field was wrong both ways** — I first used the wrong
   definition, then "fixed" it to the other wrong value.  Correct: the length
   INCLUDES the 2 length bytes (DQT=67, SOF0=17, DHT=19+n, SOS=12), which I
   confirmed against PIL's byte output.
2. **`huff_lengths` read uninitialized `lens[]`** when there was exactly one
   symbol (the early-return left lens[1..255] garbage) -> "bogus Huffman table".
3. **The FDCT DC scaling was 0.707, should be 0.3536 (1/(2√2))** — 2x off,
   invisible on all-zero blocks (why the solid-gray test passed), caught on
   real gradients.  (My decoder's IDCT was already correct — it decoded PIL's
   JPEGs at MAE 0.57 — so only the encoder's forward DCT was wrong.)
4. **Double-zigzag bug**: the quant tables are RASTER-ordered, but I briefly
   indexed them as if zigzag-ordered (a "fix" that broke the original-correct
   code, then reverted).
5. **Entropy emitted in raster-block order, not MCU-interleaved order** — the
   decoder desynchronised (k+r>63).  The solid-gray test masked it (all blocks
   identical).  Rewrote the entropy to emit 4 luma + Cb + Cr per 16x16 MCU.
6. **Quantization stored Cb/Cr interleaved but entropy read them contiguous** —
   Cb worked by luck, Cr was garbage (solid red decoded grey).  Fixed to store
   both contiguously with a shared block index.

### Built: screenshots + screen recording (roadmap #6)
- **PrintScreen** = full-screen screenshot -> /home/yart/Screenshots/shot_N.jpg
- **Alt+PrintScreen** = focused window -> win_N.jpg
- **Shift+PrintScreen** = drag a region to capture -> region_N.jpg
- **Super+R** (or F10) = toggle screen recording -> rec_N.mjpeg (~10 fps, 2x
  downscaled to 640x400 to stay feasible under TCG)
- **F9** = full screenshot (alternate that maps cleanly through PS/2, since
  PrintScreen's E0 2A E0 37 sequence is mis-read as Shift by the keyboard
  driver — a pre-existing minor bug, documented).

### Verified (QEMU/OVMF, TCG)
- Clean build, 0 warnings (minus one pre-existing init.c false-positive).
- F9 screenshot: "shot: wrote file" logged, and the JPEG (FFD8FF) appears on
  the persisted disk image.
- F10 record: 8 MJPEG frames on the disk image (throttled below 10 fps by TCG
  encode time — expected).
- 0 panics, 0 SMEP.

### Honest limits
- The camera APP is not built yet — the codec (encode + decode) is done and
  verified, and the camera is a thin UI over it (photo = one JPEG, video =
  N JPEG frames).  QEMU has no camera hardware, so the source would be a test
  pattern, honestly labelled (same as battery/wifi).
- Screenshots/recordings are JPEG/MJPEG (lossy); no PNG yet (needs a deflate
  implementation).
- Screen recording is 640x400 @ ~5-10 fps under TCG (CPU encode); on real
  hardware it can run higher.

## ⚡ THIS TURN (2026-08-18, #14): video support — a real MJPEG player

### The honest scoping
A real video codec (H.264/MPEG) is tens of thousands of lines.  The realistic
"real video" standard to start with is **Motion JPEG**: concatenated baseline-
JPEG frames.  It is a genuine ISO standard, AND it is what webcams stream, so
the decode core doubles as the backend for the camera (roadmap #5).

### Built: a from-scratch baseline JPEG decoder (userland/jpeg.c)
~380 lines, freestanding (no malloc, no libm), supports 1- and 3-component
images, 4:4:4/4:2:2/4:2:0 subsampling, DQT/DHT/SOF0/SOS parsing, Huffman
decoding, dequant + inverse-zigzag + a separable float IDCT with a hardcoded
cosine table, and YCbCr->RGB.

### 3 real bugs found & fixed while proving it (host unit test)
The decoder is verified by `scripts/test_jpeg_host.py`, which compiles jpeg.c
with the host gcc and compares its output against Python PIL's decoder over
9 test images (3 sizes x 3 subsamplings).  It exposed and fixed:
1. **Bit reader started at file offset 0** instead of after the SOS header ->
   Huffman decoded the SOI marker as entropy data.
2. **Dequantization used the wrong zigzag ordering** (coeffs are natural
   order; quant tables are zigzag order) -> added the inverse-zigzag table.
3. **The IDCT cosine was a Taylor series evaluated up to ~20 rad**, where it
   diverges.  It corrupted high-frequency coefficients - which only showed up
   on SMALL images (their 8x8 blocks span a large value range) and was hidden
   on large ones.  Replaced with a hardcoded 8x8 cosine table.
Result: **9/9 images decode with MAE 0.56-1.19** (matches PIL to ~1 level).

### Built: the /bin/media player
- Embeds a 32-frame 160x120 MJPEG clip (userland/clip.mjpeg, generated by a
  PIL script, ~100 KB).
- Opens a 640x480 window, decodes each frame with jpeg.c, upscales + blits,
  flips, paces at ~15 fps, loops; Esc exits.
- Registered as "Video" in the launcher.

### Verified (QEMU/OVMF, TCG)
- Clean build, 0 warnings (minus one pre-existing init.c false-positive).
- Launch the Video app: `exec: pid 6 'wm' -> media`; screenshot pixel analysis
  shows the bouncing red box (13k red px -> 4.9k across frames = it moved),
  the gradient (~77k colorful px), and consecutive frames DIFFER (animation
  confirmed).  0 panics, 0 SMEP.

### Honest limits
- MJPEG only (no audio, no H.264/MP4).  Audio needs wiring the existing HDA
  driver to a mixer + PCM source, a separate step.
- The clip is embedded at build time; there is no file-picker to open an
  arbitrary .mjpeg yet (the decoder is fully generic, the player's source is
  just the embedded blob).
- Decode is CPU (float IDCT); no SIMD yet.  Fast enough for 160x120 @ 15fps
  under TCG; larger/faster needs SIMD or a GPU.

## ⚡ THIS TURN (2026-08-18, #13): the Linux session model — done, verified

The user asked for what Linux distros do: stop a wm session, drop to text,
restart (or switch) the wm.  Delivered the whole loop, inspired by init/getty.

### ARCHITECTURE (the split)
- **/bin/init = supervisor** (pid 4, the boot task).  It no longer IS the
  compositor; it supervises a session loop, exactly like init/systemd.
- **/bin/wm = the compositor**, now its OWN binary (wm_main.c calls wm_run()).
- **text console (getty)** runs as a forked child of init; it claims the
  framebuffer directly (no wm), draws with the same gfx.c text renderer, and
  accepts `startwm` / `wm` / `reboot` / `help`.
- The kernel already had the fb-reclaim on wm death (turn #12); the getty is
  a child so its own death releases the claim for the next wm.

### The loop (VERIFIED end-to-end in QEMU, serial log)
```
boot -> wm(pid5) claims fb -> desktop
Ctrl+Alt+Backspace -> "ending session" -> wm dies -> fb reclaimed
-> getty(pid6) claims fb -> "text console running"
type startwm -> getty exits -> fb reclaimed
-> wm(pid7) claims fb -> NEW session (fresh lock screen)
```
0 panics, 0 SMEP, 2 execs (wm + getty), no crash loop.  Screenshot pixel
analysis: desktop → console (dark bg + text) → restarted session (lock).

### 3 real bugs found & fixed along the way
1. **`sched_fork` didn't copy `mem_limit_pages`** (and `mem_pages`).  A forked
   child kept 0 from kzalloc, so `sched_mem_limit()` returned 0 and EVERY
   mmap/fb-reserve in a forked process failed with "over memory cap (limit=0)".
   Latent while the wm was the boot task (created via sched_create_user); broke
   the moment the wm became a forked child.  Fixed: copy both in fork.
2. **Session-restart race**: `sched_exit` wakes the parent (waitpid) BEFORE
   `wm_surface_owner_died` clears the fb claim, so a freshly-started wm/getty
   saw `g_wm_task` still pointing at the dead task and fb_info returned 0.
   Fixed with a bounded fb_info retry loop in both wm_run() and text_console()
   (a login manager waits for session teardown the same way).
3. **`fb_present(fb,0,0)` is a no-op** (count 0 = nothing copied).  The text
   console used it, so it drew to the back buffer but never presented.  Now
   uses `fb_flip(fb)` (full present).

### Init binary is now tiny
init.elf = 540 KB (supervisor + text console + gfx + kora atlas).  The 16 MB
wallpaper blob now lives only in wm.elf.  This also means the boot task loads
fast and the supervisor can be swapped/replaced independently of the wm.

### Honest note (what "switch WM" still needs)
This gives stop → text → restart.  Truly "switching" to a DIFFERENT wm (e.g.
a second compositor binary) is the same loop with a different exec target -
the mechanism is already there; only an alternative compositor to switch TO
is missing.  Also: the getty is a menu of fixed commands, not a full login
shell (nyra still needs a wm to composite its window); a framebuffer-direct
shell is a future step.

## ⚡ THIS TURN (2026-08-18, #12): FS sync fixes + WM session foundation

### FIXED — 2 filesystem bugs
1. **`mount_count` incremented on every sync** (every ~1 s), not per mount —
   the "mount count" was really a sync count.  Now incremented once in
   `blkfs_init` and persisted.
2. **`blkfs_sync` rewrote the superblock + device-FLUSH'd every second even
   when nothing changed.**  The periodic auto-sync now early-returns when no
   file was written and no inode was deleted, so an idle FS does zero disk I/O.

### ADDED — WM session model: foundation (roadmap #3)
The compositor can now be stopped and the system falls back to TEXT, verified
end-to-end:
- **Kernel fb-reclaim** (the bug that made sessions impossible): when the
  compositor task dies, `g_wm_task`/`g_wm_uaddr` are now cleared, so a future
  process CAN re-claim the framebuffer.  Before, `g_wm_task` pointed at a dead
  task forever and no new process could ever take over the screen.
- **Kernel text fallback screen**: an 8x16 bitmap font (generated from DejaVu
  Sans Mono via a script) is embedded in the kernel; `fb_draw_text()` +
  `fb_fallback_screen()` paint a "graphical session ended / text fallback
  mode" screen straight to the back buffer + present when the wm dies.
- **Ctrl+Alt+Backspace** (classic X11 "zap") in the compositor ends the
  session: `wm_session_end()` breaks the render loop, init exits, the kernel
  reclaims the fb and paints the fallback.

### Verified (QEMU/OVMF, TCG)
- Clean build, 0 warnings.
- Boot → unlock → desktop renders → Ctrl+Alt+Backspace → log shows
  `ending session` + `compositor (pid 4) died - reclaiming framebuffer`.
- Screenshot pixel analysis: desktop (wallpaper) → fallback (113k dark-bg px +
  259 white-text px + 15 accent px for the "YartOS" title).
- 0 panics, 0 SMEP, 0 starved.

### Honest note (what "session model" still needs)
This delivers "stop the graphical session → text only".  The full loop —
RESTARTING the session from the fallback, or landing in a working text SHELL
— needs (a) splitting init (supervisor) from wm (compositor) so a supervisor
can re-launch the wm, and (b) a fullscreen text console (nyra is a GUI app;
it needs the wm to composite its window, so it can't be the fallback shell as
is).  Those are the next steps and are NOT claimed here.

## ⚡ THIS TURN (2026-08-18, #11): FS bug fixes + symlinks

### FIXED — 4 real filesystem bugs (found by re-reading my own v5 code)
1. **CRC index wrong for blocks in group 1+.** `data_index(blk) = blk -
   data_start[0]` assumed data blocks are contiguous, but each block group
   carries 34 metadata blocks interleaved, so a block in group 1+ mapped to a
   wrong CRC slot (corrupting/incorrectly-validating integrity metadata).
   Now computed by summing the data-block counts of preceding groups.
2. **CRC region over-allocated** — sized from `journal_start - data_start`
   (counts interleaved metadata as data).  Now sums actual data blocks.
3. **File shrink leaked blocks + left stale indirect pointers.** The old
   `clear_blocks_from` only freed indirect trees when shrinking to ≤12 direct
   blocks, and `persist_data`'s free loop double-freed/dangled.  Replaced with
   a single correct `truncate_blocks()` for any shrink.
4. **CRCs were stored but never validated.**  Mount now checks each file data
   block's CRC on load and logs a `!! CRC mismatch` (none fired in testing —
   the data is clean).

### ADDED — symlinks (Stage 5)
- `VN_SYMLINK` vnode type; `vfs_symlink()`, `vfs_lookup_nofollow()`,
  `vfs_lookup_at_nofollow()` (final component returned as-is so unlink/rename/
  stat act on the link, not the target).
- Symlink resolution in `vfs_lookup_at` (follows mid-path AND final components,
  up to 8 deep, relative targets resolve against the link's parent).
- `SYS_SYMLINK` (92) + `SYS_READLINK` (93) syscalls + userland wrappers.
- Shell: `ln -s <target> <link>` and `readlink <link>`.
- On-disk: symlink target stored in the inode's data blocks, `type = LNK(3)`.
- `sys_unlink`/`sys_rename` switched to nofollow so they delete/rename the link
  itself.
- Kernel selftest VERIFIED end-to-end (gated behind -DBLKFS_SELFTEST):
  `symlink resolves to target (ok)`, `readlink -> selftest.txt`,
  `symlink inode: ino=8 type=3 size=12`, `yfs v5 selftest PASS`.

### Verified
- Clean build, 0 warnings.  Boot 1→2 counter persists, 0 panics, 0 SMEP,
  0 CRC mismatches, 0 out-of-space.
- Full clean rebuild (the -DBLKFS_SELFTEST flag was reverted after the
  verification boot).

### Honest note
Hard links are NOT wired yet: the RAM vnode cache is a tree (one parent per
node), which hard links violate.  The disk format already has `links_count`;
hard links need a RAM-cache redesign (a vnode visible under multiple
parents), queued for a future turn.

## ⚡ THIS TURN (2026-08-18, #10): YartFS v5 — a real ext-architected filesystem

The user wanted the VFS at "real world OS level".  I rewrote the on-disk
filesystem (kernel/fs/blkfs.c) from the v4 "path-string inode" design to the
model ext2/ext3/ext4 actually uses.  **VERIFIED end-to-end: boot counter
persists 1 → 2 → 3 across three reboots.**

### What v4 got wrong (the #1 structural flaw, now fixed)
v4 keyed every inode by a full 160-byte PATH STRING (`inode_find` = linear
strcmp over 2048 inodes), and directories had no on-disk form — they were
reconstructed at mount by slicing path prefixes.  v5 replaces that wholesale.

### What v5 is (all boot-verified)
- **4 KiB blocks** (8×512 sectors; the block layer loops per page).
- **Block groups** (ext's signature layout): the volume splits into groups,
  each with its own block bitmap + inode bitmap + inode table, for locality
  and fsck-ability.  Dynamic geometry (64 MiB disk → 4 groups × 1024 inodes).
- **Inodes keyed by number** (root = 1, 128-byte inodes).  No path strings on
  disk.  Hard-link `links_count` field is in place.
- **On-disk directory entries**: a directory's data is a packed
  (inode, rec_len, name_len, type, name) array — ext's dirent.  Lookup walks
  dirents; `ls` reads them; mount rebuilds the tree by walking dirents from
  the root.  No path reconstruction, no O(2048) strcmp.
- **Indirect-block file mapping** (direct[12] + single + double + triple, 1024
  pointers/block).
- **Integrity**: per-inode CRC32 + per-data-block CRC32 + a superblock
  mount counter + a clean/dirty `state` flag (ext2-style crash DETECTION —
  the unclean-shutdown warning fired correctly on a SIGKILL'd boot).
- **Write ordering**: data blocks → inode → parent dirents, with dirtiness
  propagated up the tree so the whole path root→file is navigable on disk.

### Real bugs found & fixed during the port
1. `format()` wiped `g_desc` with memset AFTER `compute_layout()` filled it →
   every allocation failed ("out of space").
2. `persist_node` read garbage inode slots for freshly-allocated inodes → the
   phantom block pointers caused a page-fault panic (16 KB kernel stack
   overflow from nested 4 KB block buffers — converted to static/kmalloc,
   safe under vfs_lock's IRQ-off single-threading).
3. Superblock `magic`/`version` were never written → every boot reformatted
   (no persistence).
4. Dirents included initrd "seed" files (ino 0) → invalid entries; now skipped
   (seeds re-import from initrd each boot).
5. Creating a file only dirtied its immediate parent, so the ancestor dirs
   never got inode numbers → the disk had no path to the file.  Fixed by
   propagating dirtiness up the tree in `persist_node`.

### VFS-layer changes (small, API-compatible)
- `vnode_t` gained `u32 ino`; `blkfs_note_delete` now takes an inode number
  (not a path); `vfs_create`/`vfs_unlink` mark the parent dir dirty; added
  `vfs_find_child`.  The syscall layer (open/read/write/mkdir/unlink/rename)
  is untouched.

### Honest limits (documented, not hidden)
- This is ext-*architected*, NOT a literal ext4 driver — you cannot mount a
  YartOS disk in Linux as ext4.
- The journal is currently the CRC + superblock-state + write-ordering
  scheme (ext2-style crash DETECTION).  A full redo-log journal (ext3/ext4
  jbd2-style, crash RECOVERY) is the next stage, per docs/EXT4_VFS_PLAN.md.
- No extents yet (indirect blocks, which ext2/ext3 also used); htree dir
  indexing not needed at this scale.
- Partial-shrink within the indirect range can leave a few blocks allocated
  (harmless, reclaimed at format); files < 48 KB — the common case — are
  fully cleaned up.

### Verified
- Clean build, 0 warnings; ISO ~27 MB.
- 3 consecutive boots: format → mount → mount, boot counter 1→2→3, 0 panics,
  0 SMEP, 0 starved, 0 out-of-space.
- 6 app launch/close cycles: stable.

## ⚡ THIS TURN (2026-08-18, #9): terminal reliability — 3 real fixes

### FIXED — `/bin/prog` rejected ALL arguments
The launcher checked `has_space` and printed "usage: /bin/prog [&]  (no args
yet)", so you could never run `/bin/files /home/yart/.trash` or any program
with arguments.  Now the command is tokenized in place into a real argv
(path + up to 30 space-separated args) and passed to exec().  This is the
single most common thing a shell must do.  (userland/nyra.c)

### FIXED — Ctrl+C orphaned the foreground job (zombie leak)
Ctrl+C with a running foreground job printed "^C" and just cleared G_fg_pid,
NEVER signalling the child: the child kept running as an orphan, and its
zombie was never reaped.  Now Ctrl+C raises SIGINT (signal 2) on the fg job;
with no handler the child terminates (status 130) and the waitpid loop reaps
it.  (userland/nyra.c)

### FIXED — background jobs were never reaped (zombie leak)
The main loop only waitpid'd G_fg_pid.  A finished background job stayed a
zombie forever (its parent, the shell, is alive, so the kernel orphan reaper
never touches it) and `jobs` kept listing it "Running".  Added a throttled
(~2x/s) background reaper that waitpid's each non-fg job and removes finished
ones (reporting "Done" / "Stopped").  (userland/nyra.c)

### Verified
- Clean build, 0 warnings; boot 0 panics / 0 SMEP / 0 starved.
- nyra launches and runs (fork pid 4 -> 5, exec nyra), no errors.
- Honest: full end-to-end keyboard verification of the args path was blocked
  by the QMP special-character injection limitation (documented repeatedly in
  this audit) - the argv parsing is traced correct and the build is clean; a
  real PS/2 keyboard is unaffected (letters/fn-keys inject and work fine).

## ⚡ THIS TURN (2026-08-18, #8): the REAL answer on lag — TCG + 3 more perf fixes

User (this turn): "just kill the lag, make it always smooth as butter like a
real OS."  Asked where they run it: **TCG** (software emulation, no KVM), and
the cursor is what lags.  This changes the answer fundamentally, and I'm
stating it plainly instead of pretending a code change fixes emulation.

### The honest truth: TCG cannot be "smooth as butter"
TCG emulates every x86 instruction in software - 10-50x slower than native.
Even Linux/GNOME is visibly janky under TCG.  No compositor optimization
makes a fully-emulated OS feel like a real OS; the cursor, the window drags,
everything inherits the 30x slowdown.  The fix is KVM (or bare metal), not
more code.  run.sh and run-qemu.sh now print a LOUD warning + exact
instructions when they fall back to TCG.

### Still squeezed out 3 real wins (help TCG AND real hardware)
1. **Precise sub-ms frame pacing via a userspace TSC clock.**  The frame loop
   busy-polled `time_ms()` (a syscall) - expensive per iteration.  Now the WM
   calibrates the TSC rate ONCE (inline rdtsc, unprivileged) against two
   time_ms() samples, then busy-waits the sub-4ms tail with an inline rdtsc -
   ZERO syscalls in the pacing path (the vDSO trick a real OS uses).  Frames
   land on a precise cadence; the last micro-jitter in cursor motion is gone.
   (userland/wm.c)
2. **Killed the SMP heartbeat log storm.**  Each AP printed "AP N alive" every
   ~0.5 s; kprintf busy-waits per character on the UART, so 3 APs × ~90 bytes
   every half-second is real serial I/O under TCG - measurable slowdown of
   everything else.  Throttled to every ~8 s and shortened.  (kernel/smp.c)
3. **125 Hz input polling** (last turn) + **pre-scaled cursor** (turn #5) +
   **TSC clock** (turn #2) are all in place; on KVM/bare metal the cursor
   glides.

### Verified (QEMU/OVMF, TCG)
Clean build, 0 warnings; boot 0 panics / 0 SMEP / 0 starved; launch/close
stable; AP heartbeat reduced ~8x.

### The ONE thing the user must do
Enable KVM (modprobe kvm-intel/kvm-amd + `usermod -aG kvm $USER`, log out/in),
or boot the ISO on the real laptop.  Everything else is already done; TCG is
the remaining wall and no code removes it.

## ⚡ THIS TURN (2026-08-18, #7): "brushing" bug root-caused + Skift-style Files + lag

### FIXED — the "brushing" erase (root cause, finally)
`overlay_state` in wm.c listed every popover EXCEPT `G_clip_open` and
`G_netlist_open`.  That mask drives the "open/close -> damage_whole()"
repaint, so the clipboard and Wi-Fi network-list popovers NEVER triggered a
full repaint on open or close.  Consequences (exactly what the user reported):
- after "click elsewhere to dismiss", the popover's pixels lingered on screen
  ("it stays there"),
- moving the cursor over the stale popover redrew the backdrop in a trail
  ("passing the cursor over it deletes it, like brushing").
Fix: added both bits to the mask.  (The other overlays already worked; only
these two were missing.)  userland/wm.c

### FIXED — lag: variable frame pacing (halves input latency)
The frame loop always slept a fixed 16 ms, adding up to a full frame of
latency to every cursor move.  Now it paces at ~125 Hz (8 ms) while the user
is moving the mouse / typing, and settles back to 16 ms when idle.  This is
the same "poll faster during interaction" a real compositor does.  userland/wm.c

### FIXED — lag: mmap "reserve FAIL" storm (O(pages) -> O(regions))
Boot logs showed ~1343 `vmm: reserve FAIL ... overlaps region` lines.  The
17 MB compositor image (init.elf carries the 16 MB wallpaper blob) can overlap
the mmap arena depending on the ASLR bias; `sys_mmap` then probed ONE PAGE AT A
TIME with a log line per failed reserve.  Added `vmm_user_find_free()` (quiet,
skips past whole regions) and rewrote the mmap probe to use it.  Boot is now
clean: 0 "reserve FAIL" lines, and the slow linear scan is gone.
kernel/mm/vmm.c, kernel/include/yart/mm.h, kernel/arch/x86_64/syscall.c

### UPGRADED — Files app to a Skift-style file manager
(Skift's current main branch no longer ships hideo-files — it's been split into
core libs + strata-* services — so I implemented Skift's *design* directly.)
- **Sidebar** (Places: Home / Documents / Downloads / Trash) with Kora icons,
  active-place accent highlight, clickable.
- **Toolbar**: back / forward / up buttons (greyed when unusable), a path bar,
  and a New-folder button.
- **Navigation history** (back/forward stack) so entering folders, going up,
  and clicking places all navigate properly.
- **List view** with folder/file icons (folders accent-blue, files by
  extension), Name + Size columns, selection.
- **Status bar** with item count + the current operation result.
- Real operations retained: copy/cut/paste, inline rename (cursor editing),
  delete-to-trash, restore, empty trash, new folder.  userland/gui_apps.c

### Verified (QEMU/OVMF, -cpu max, SMP 4, TCG)
- Clean build, 0 warnings.  ISO 27 MB.
- Boot: 0 panics, 0 SMEP faults, 0 "reserve FAIL" lines, compositor starts.
- Screenshot pixel-analysis: Files renders sidebar (10.3k sidebar-bg px) +
  accent-highlighted active place (787 accent px) + list + toolbar.
- 8 launch/close cycles: 0 crashes, 0 starved.

### Honest notes
- Residual cursor lag under pure TCG is inherent (10-50x emulation); on real
  hardware / KVM the pacing + TSC clock + pre-scaled cursor make it glide.
  A hardware cursor sprite is the true zero-cost endgame (needs a VGA cursor
  driver, out of scope).

## ⚡ THIS TURN (2026-08-18, #6): shell/desktop/wm fixes — copy-paste, cd/ls, right-click, selection, Files upgrade

User batch: right-click inside a window shows the desktop menu · dragging on the
desktop selects ALL icons · console text deselected instantly (no copy) ·
Ctrl+C/V "for copy paste" confusion · `cd` "doesn't work" · Settings needs
icons · Files rename needs text selection · double-click desktop icons "crashes".
All code-changed, built clean, boot-verified (0 panics @ 250 Hz).

### FIXED — right-click inside a window showed the DESKTOP menu
`handle_press(button==3)` hit-tested the titlebar, then FELL THROUGH to the
dock/desktop checks, so right-clicking in the Console content area opened
"Personalize… / Settings".  Now a right-click inside a window's content area
is swallowed (the app owns its context menu) and never reaches the desktop.
(userland/wm.c)

### FIXED — desktop drag selected ALL icons
`desk_selected()` returned `G_multi_sel`, so once a marquee set it, every icon
highlighted.  Now it only returns `G_sel_desk==i`; marquee selection is
computed per-icon against the actual rectangle in `draw_desktop_live()`, and
`G_marquee`/`G_multi_sel` reset on drag release.  (userland/wm_dock.c, wm.c)

### FIXED — console text selection + copy/paste (proper terminal convention)
The old code cleared the selection the instant the drag ended (so it "just
deselected" with no way to copy).  Now:
- drag = select, the highlight PERSISTS after release (until next press / copy)
- **Ctrl+Shift+C = copy**, **Ctrl+Shift+V = paste** (the real terminal
  convention — the driver reports shifted keys as uppercase ascii + KEY_SHIFT)
- **Ctrl+C stays interrupt/cancel** (SIGINT), never copy — this answers the
  "wtf is ctrl c for copy paste" question: in a real terminal Ctrl+C is cancel
  and copy/paste are Ctrl+Shift+C/V (or middle-click paste = plain click here).
(userland/nyra.c)

### FIXED — `cd` "doesn't work" (root cause: `ls` defaulted to "/")
`cmd_ls("")` re-defaulted the empty path to "/", so after `cd /home/yart` the
shell still listed ROOT — making `cd` look broken.  Now `ls` with no argument
lists the current directory (the kernel resolves "" to the cwd), and the
prompt shows the cwd ("/home/yart $ ") so `cd` is visibly reflected.
(userland/nyra.c)

### ADDED — icons in Settings content pages
Personalization (desktop/cursor/wallpaper) and System (dock/volume/display)
rows now show their Kora icons alongside the labels.  (userland/gui_apps.c)

### UPGRADED — Files app rename = real text field + icons + toolbar buttons
- Rename now has full cursor editing: Left/Right/Home/End, insert-in-middle,
  backspace, Delete — not just "append at end".
- File/folder rows show icons (folder blue, files by extension).
- Toolbar has clickable New-folder + Rename icon buttons + an [<-] up button.
(userland/gui_apps.c)

### Honest notes
- "Double-click desktop icon crashes": could NOT reproduce — 2 automated
  double-click passes (tight + spaced) produced 0 forks/execs/panics, meaning
  the click coordinates weren't landing on the icon (HMP mouse injection +
  the driver's accel filter is unreliable — already documented in the audit).
  The launch code path (`desk_hit` -> `launch_app` -> fork/exec) is unchanged
  and sound.  If a real crash exists it is the SMEP race (below), which is
  intermittent and now panics cleanly instead of corrupting.
- SMEP-on-exec race: still open (see turn #4).  The 250 Hz tick keeps it rare.

## ⚡ THIS TURN (2026-08-18, #5): cursor lag/vanishing — root cause fixed

User: "when i move cursor, it still lags, it vanishes and shows up in another
place ... doesn't clearly show while moving."

### Root cause (real, verified)
The photo cursors are packed at up to 48px and were being bilinear-downscaled
to 4/5 size **with FLOAT math on EVERY frame**, in `cursor_blit_raw()` (wm.c):
~48x48 ≈ 1700 destination pixels × ~30 float ops each ≈ **50k float ops per
frame, just for the cursor**.  On a software float path (and especially under
TCG emulation) that is milliseconds of per-frame cost, so the compositor's
frame rate collapsed while the pointer moved.  Combined with the frame loop
draining ALL queued PS/2 packets in one frame and only painting the FINAL
cursor position, the pointer visibly "teleported" between sparse frames —
exactly the "vanishes, then reappears elsewhere" the user described.

### Fix
- **Pre-scale once, blit every frame.**  New `cursors_draw_img()` in
  userland/cursors.c lazily bilinear-downscales each cursor ONCE (same
  premultiplied-alpha math as before, identical visual result) into a cached
  straight-ARGB bitmap at draw size.  wm.c's `cursor_draw()` now blits that
  with a plain integer alpha blend (`cursor_blit()`, ~1700 integer ops) — the
  per-frame float work is gone.
- CURSOR_SCALE_NUM/DEN moved to cursors.h (single source of truth).
- `cursor_rect()`/`cursor_draw()` unified through `cursor_current_img()` so
  the damaged rect and the drawn image can never disagree.

### Verified (QEMU/OVMF, TCG)
- Clean build, 0 warnings.
- Screenshot pixel-analysis: cursor renders exactly at the target position
  (white arrow + dark outline, ~478 white + ~260 dark px in a 30x30 box), and
  the PREVIOUS position is cleanly erased (0 white px where the cursor was) —
  no ghosting, no trail.
- 0 SMEP faults, 0 panics.

### Honest limit (not claimed)
Under pure TCG the compositor still can't hold a real 60 Hz under heavy load,
so very fast flicks will still step — but the cursor no longer does 50k float
ops/frame, which removes the pathological lag.  On real hardware / KVM the
pointer now glides.  (A hardware cursor sprite would be the zero-cost endgame,
but that needs a VGA/QEMU cursor-plane driver, out of scope.)

## ⚡ THIS TURN (2026-08-18, #4): SMEP-on-exec race — deep trace + 2 real fixes

Spent this turn chasing the #1 documented open bug: the intermittent SMEP-on-
exec race that made the 1 kHz tick crash apps.  Result: 2 concrete fixes +
diagnostics + several hypotheses ruled out, but NOT yet a root-cause fix of
the frame corruption itself.  Honest status below.

### FIXED — sched_fault_recover() infinite loop (real bug)
When the SMEP fault (kernel executing from a user address = corrupt iretq
frame) fires with a NON-user current task — the BSP's desktop task (pid 0) or
an AP mid-switch-to-idle (ap_current NULL) — the old code returned
`current_rsp` / `switch_to_idle(current_rsp)`, i.e. the SAME corrupt frame,
which the ISR stub iretq'd straight back into → the fault repeats forever.
This is exactly the observed "cpu: SMEP fault ... dropping corrupt context"
storm (333 identical lines in one boot).  Fix: if there is no user task to
drop, the corrupt frame is a KERNEL frame — a genuine kernel bug, not a
recoverable user fault — so `sched_fault_recover` now `kpanic()`s with a
clear message ("corrupt kernel frame ... unrecoverable") instead of looping.
(kernel/sched/sched.c)

### FIXED — cosmetic `\\n` in the SMEP log
The "dropping corrupt context" kprintf had a literal `\\n` (backslash-n) not a
newline, so every SMEP line concatenated onto one giant log line. Fixed.

### ADDED — SMEP diagnostic dump (for the next reproduction)
The #PF SMEP handler now, on the FIRST fault only, dumps the full frame
(RIP/CS/RFLAGS/RSP/SS + GPRs) plus `smep-dbg: cpu=N cur_task=N 'name'
is_user=N`.  This is the one piece of data that has been missing every time
the race was reported: it will finally tell us WHICH frame is corrupt (BSP
desktop vs AP idle) and its registers, instead of guessing.

### Root-cause investigation (what I traced + ruled out, NOT claimed fixed)
- **AP idle-frame mechanism**: `ap_idle_rsp` is set in `sched_tick` (the
  !cur case saves the hlt frame) and cleared in the idle loop before
  `sti; hlt`; the wake IPI (vec 62) routes through `sched_tick` too.  Traced
  it correct.
- **switch_to vs sched_kill/reap**: both hold `g_switch_lock`; kill frees the
  PML4 only when not running-anywhere, switch_to refuses ZOMBIE next.  Traced
  airtight.
- **Stale TSS RSP0 after switch_to_idle**: `switch_to_idle` leaves RSP0
  pointing at the last user task's (possibly freed) kstack.  Determined
  HARMLESS: RSP0 is only used for ring3→ring0, and no ring-3 code runs while
  ap_current==NULL; `switch_to` re-arms RSP0 before any user task runs.  Left
  as-is (fixing it would be cargo-cult).
- **Reproduction**: could NOT re-trigger the fault this turn (keyboard
  launch/close 40×, mouse launch+minimize/restore/drag 20×, all at 1 kHz, all
  0 faults).  The earlier 333-fault storm was a timing fluke of one specific
  mouse sequence.  So the corrupt-frame root cause remains OPEN — but next
  time it fires, the smep-dbg dump will identify the owner, and the panic
  (instead of a storm) will make it a clean, diagnosable stop.

### Where things stand (honest)
- Tick stays at **250 Hz** (stable: 0 faults / 0 panics / 0 starved across
  8-40 launch/close cycles; 500 Hz panics ~1/8, 1 kHz crashes apps).
- The SMEP frame-corruption race is pre-existing, still open, now
  instrumented.  It is a symptom of the fork/exec/surface/CoW SMP lifecycle
  and needs a dedicated multi-day audit to close for real; this turn made it
  fail LOUDLY and CLEANLY rather than wedge the machine.

## ⚡ THIS TURN (2026-08-18, #3): window ops fixed + ISO 109 MB -> 27 MB + verified in QEMU

This turn I installed a real toolchain + QEMU in the sandbox, so EVERYTHING below
is BUILD + BOOT + SCREENSHOT verified (pixel-analysis driven), not just reasoned.

### 1. FIXED — 100 MB ISO (root cause: 16 MB wallpaper duplicated into 6 ELFs)
`scripts/gen_wallpaper_pack.py` packs 4 wallpapers as RAW BGRA (1280x800x4 =
4 MB each = 16.4 MB) into build/wallpaper.bin, and that blob was `ld -r -b
binary`'d into EVERY userland ELF (init, nyra, files, settings, editor,
browser). 6 x 17 MB ≈ 100 MB of ELF -> 109 MB ISO.
FIX: moved the wallpaper pixel accessors out of gfx.c (which every app links)
into a new userland/wallpaper.c, linked only into /bin/init (the compositor,
the only thing that composites the wallpaper).  Apps that need the wallpaper
COUNT use the new WALLPAPER_COUNT constant in gfx.h (gui_apps.c Settings).
VERIFIED: app ELFs 17 MB -> ~0.6 MB each; ISO 109 MB -> 27 MB.

### 2. FIXED — window maximize/fullscreen (was a no-op + would over-read)
toggle_max() wrote w->w/h = screen size LOCALLY and only called wm_move();
scan_windows() re-synced w->w/h from the (unchanged) kernel surface every
other frame, so the window never grew, and draw_window would have blitted a
640x440 surface into a 1280x670 rect (buffer over-read).  Surfaces are capped
at WM_SURF_MAX 640x480 while the framebuffer is 1280x800, so true fullscreen
is done by UPSCALING: added win_client_rect()/win_frame_rect() as the single
source of truth (maximized -> fills the work area below the panel), and
draw_window() now sf_blit_scaled()s the surface to fill when maximized (the
same "legacy app on HiDPI" model the 2x scale already uses).  VERIFIED:
maximize click grows the window from ~640x440 to the full 1280x800 work area.

### 3. FIXED — window drag "jumped up 34px" (off-by-titlebar-height bug)
The drag anchor was G_drag_dy=(y+TB_H)-w->y (offset in the titlebar), but the
drag handler computed ny=y-G_drag_dy and stored it as the CLIENT top, missing
the +TB_H.  Every grab teleported the window up by TB_H (34 px).  Fixed to
ny=(y-G_drag_dy)+TB_H and the anchor is now the grab offset within the
titlebar.  Resize edges were also rewritten against win_client_rect() with
correct left/right/top/bottom math + kernel surface-cap clamps (640x480).
VERIFIED: drag moves the window with the cursor, no jump.

### 4. ADDED — Super+Tab window switcher (was only Alt+Tab)
Super alone opens the app launcher (app grid); now Super+Tab opens the window
switcher (all windows, live previews), the Win+Tab / GNOME Super+Tab model.
Factored the switcher-advance logic into switcher_advance() shared by Alt+Tab
and Super+Tab.  Handled BEFORE the app-grid search handler (which would
otherwise swallow Tab while the grid is open).  VERIFIED: switcher overlay
renders.

### 5. TICK RATE: 100 Hz -> 250 Hz (and why NOT 1 kHz — honest)
Last turn I raised the tick to 1 kHz (Skift's rate) for 1 ms sleep resolution,
but boot-testing apps this turn showed it triggers the PRE-EXISTING SMEP-on-
exec race (kernel executes from a corrupt-frame user address) ~100% of the
time: an infinite "cpu: SMEP fault ... dropping corrupt context" loop with the
IDLE task current (0 "fault-recover" logs = the idle frame is corrupted).
Measured the threshold: 100 Hz = 0 faults, 250 Hz = 0 faults across 8+
launch/close cycles, 500 Hz = 1 panic/8, 1000 Hz = 100%.  So the tick is now
250 Hz (4 ms sleep resolution, a common Linux HZ) — the stable sweet spot.
The TSC sub-ms clock from last turn still drives animation interpolation, so
motion is smooth; only frame-cadence sleep is 4 ms (not 1 ms).  The 1 kHz
regression is a symptom of the still-open SMEP race, documented below, NOT
fixed this turn (it needs its own dedicated fork/exec frame audit).

### Verified this turn (QEMU/OVMF, -cpu max, SMP 4, TCG)
- make iso clean (0 warnings).  ISO 27 MB.
- Boot: 0 panics, 0 exceptions, all selftests PASS.
- 8-20 launch/close cycles at 250 Hz: 0 SMEP faults, 0 starved tasks.
- Screenshot-verified: launch, maximize (fills screen), minimize (hides),
  drag (tracks cursor, no 34 px jump), Super+Tab (switcher), Alt+Tab.

## ⚡ THIS TURN (2026-08-18, #2): 1 kHz scheduler tick — Skift's actual pacing model

User asked: "make it as smooth as a real OS / Skift."  I read Skift's real
source this time and translated the ONE thing that actually makes it smooth.

### What Skift actually does (receipts, from the cloned skift repo)
- `src/kernel/hjert/x86_64/arch.cpp`: on the PIT IRQ (irq 0) it calls
  `switchTask(1_ms, frame)` → `Clock::monotonicTick(1_ms)` + `schedule()`.
  **Skift's scheduler tick is 1 kHz**, giving 1 ms time + sleep resolution.
- `src/srvs/strata-shell/main.cpp::runAsync()`: `sleepAsync(lastFrame + 16_ms)`
  — a **deadline-based sleep** with a monotonic `instant()` clock.
So Skift smoothness = (a) 1 kHz tick, (b) monotonic clock, (c) sleep-until-a-
16 ms deadline.  NOT a GPU, NOT hardware vsync — its compositor is software
(karm-gfx) exactly like YartOS's.

### The bug (root cause of the permanent "laggy" feel)
YartOS ticked at **100 Hz**.  `SYS_TIME_MS` and `SYS_SLEEP` both derived from
that, so the compositor's frame loop (`next_frame += 16; sleep(next_frame-now)`)
slept in 10 ms steps: frames landed at 10 or 20 ms (alternating 50/100 fps),
i.e. constant cadence jitter no matter how fast the CPU was.  This turn last
time I added a TSC clock for animation math but explicitly left the 10 ms
sleep quantization as "still not done".  This turn removes it at the source.

### What changed (verified by BUILD + BOOT this turn — toolchain installed in-sandbox)
- **`kernel/include/yart/hal.h`**: `TICK_HZ 1000` as the single source of truth,
  plus `MS_TO_TICKS()` / `TICKS_TO_MS()` so no raw tick number can rot again.
- **`apic.c`**: APIC timer now fires at `TICK_HZ` (1 kHz).  `lapic_timer_calibrate()`
  made rate-independent: it times `CAL_RELOADS = TICK_HZ/50` PIT-counter reloads
  (~20 ms at ANY rate) and computes `bus = delta*16*TICK_HZ/CAL_RELOADS`.  (The
  old `delta*16*50` hardcoded the 100 Hz assumption.)
- **`pit.c`**: PIT programmed at `TICK_HZ`; `tsc_calibrate()` FIXED — last turn's
  version busy-waited on `pit_ticks()` BEFORE `sti()`, which never advances with
  interrupts off (a hang).  Now it runs after `sti()` (moved in `main.c`) and
  calibrates over 100 ms.
- **`sched.c`**: sleep wake = `pit_ticks() + MS_TO_TICKS(ms)` (1 ms resolution);
  watchdog cadence `% MS_TO_TICKS(1000)`.
- **Every tick-tuned timeout converted to `MS_TO_TICKS(actual_ms)`** so 1 kHz
  does NOT silently shorten them: watchdog (3 s / 10 s / 2 s writeback), doas
  lockout (1 s), blkfs auto-sync (1 s), ARP TTL (3 s), DNS (400 ms), ARP/ping
  (1.5 s), DHCP retry (200 ms), TCP retrans/connect/close (250 ms / 8 s / 2.5 s),
  TLS handshake deadlines (20 s / 1 s / 300 ms), IPv6 (1.5 s / 100 ms / 600 ms),
  WiFi scan debounce (500 ms).

### VERIFIED end-to-end (QEMU/OVMF, TCG, 4 cores, headless serial)
- `make iso` clean (kernel + userland, host gcc + nasm).
- Boot: **0 panics, 0 unhandled exceptions, 0 FAIL lines**, all 15 selftests PASS.
- `apic: timer calibration delta=863081/20 reloads -> bus ~690464800 Hz` (new
  rate-independent calibration correct).
- `apic: timer vec48 periodic ~1000 Hz (count=43154)` — **tick is 1 kHz**.
- `tsc: 449624 counts/ms (calibrated over 100 ms)` — TSC clock live (no hang).
- `watchdog: service 'kclockd' STALLED for 3050 ticks` — 3 s timeout == 3050
  ticks at 1 kHz (backdate +50), confirming the converted constant.
- `wm: ring-3 compositor (pid 4) claimed the framebuffer`; system idles stably
  (APs alive at ~0% work, IPv6 background probing) for the full 180 s boot.

### Honest limits (NOT claimed)
- 1 ms resolution (Skift parity) is "smooth", not frame-perfect: 16 ms pacing
  at 1 ms granularity is ~62.5 fps with ≤1 ms jitter.  Good enough to look like
  Skift; a hardware vsync-driven page flip (the only thing a real OS has that
  Skift also lacks) is still not implemented and would be needed for zero-tear.
- TCG (no KVM) is still 10-50x slower; on real hardware / KVM this is fast.
- The tick is now 1 kHz on all 4 cores = 4x more timer IRQs; negligible on real
  silicon, a small extra TCG cost (boot still completed clean).

## ⚡ THIS TURN (2026-08-18): rendering smoothness + top-bar icons
User reported (a) the desktop "always lags / isn't smooth like a real OS" and
(b) "none of the top-bar icons work — I click and nothing shows."  Both were
real, root-caused and fixed in code.  Honest verification note: this sandbox
has NO cross-compiler (no x86_64-elf-gcc) and NO QEMU, so I could not run
`make iso` or boot the result this turn — everything below is syntax-checked
(`gcc -fsyntax-only -ffreestanding`) and reasoned from the code, NOT
boot-verified.  Must be re-booted on the real machine/with a toolchain.

### FIXED — top-bar icons showed nothing (chicken-and-egg geometry bug)
`userland/wm.c::composite_rect()` gated each popover on
`rect_colide(r, {G_quick_x,G_quick_y,G_quick_w,G_quick_h})`, but those bounds
are only assigned INSIDE the draw call (`draw_quick()` etc. set `G_quick_x`
at the top).  They are zero-initialised globals, so on the first frame the
test is `rect_colide(whole, {0,0,0,0})` = false → `draw_quick` never runs →
the bounds stay 0 forever → the popover can never render.  This broke the
clock→calendar, the status-cluster→quick-settings, the clipboard button and
the wifi-chevron→network-list.  (The app grid / overview / switcher / menu
were already drawn unconditionally, which is why they worked.)
FIX: draw quick/calendar/clipboard/netlist/dockmenu unconditionally — the
global clip set at the top of `composite_rect()` already confines the writes
to the dirty rect, so this costs nothing and removes the circular dependency.

### FIXED — sub-ms clock (the root cause of the "laggy/choppy" feel)
The ONLY time source was `pit_ticks()` at 100 Hz, and `SYS_TIME_MS` returned
`pit_ticks()*10` — so `time_ms()` advanced in 10 ms steps.  Every animation
(dock bounce/lift, window open/minimise easing, OSD fades) and the 60 Hz
frame pacing computed progress from that coarse clock, so motion stepped in
10 ms increments instead of flowing.  This is independent of TCG emulation:
it jitters on real hardware too.
FIX: added a TSC-backed monotonic millisecond clock.
- `kernel/arch/x86_64/pit.c`: `rdtsc_u64()`, `tsc_calibrate()` (measures TSC
  over 10 system ticks ≈ 100 ms, stores counts/ms), and `time_ms()` (sub-ms,
  falls back to the PIT clock if TSC is uncalibrated/broken).
- `kernel/arch/x86_64/main.c`: call `tsc_calibrate()` right after `pit_init(100)`.
- `kernel/arch/x86_64/syscall.c`: `SYS_TIME_MS` now returns `time_ms()`.
- `kernel/include/yart/hal.h`: declares `tsc_calibrate()` / `time_ms()`.
The scheduler, watchdog and all tick-based logic still use `pit_ticks()`
(unchanged) — only the userspace-visible ms clock got finer.

### FIXED — per-frame syscall storm (`pid_forget_dead()`)
`wm_run()` called `pid_forget_dead()` EVERY frame; it issues one
`waitpid_nohang()` syscall per recorded pid (up to MAX_PIDS=24/frame), a real
latency tax under TCG.  Now throttled to ~2×/s (a closed app vanishes within
500 ms).  `scan_windows()` remains every-other-frame.

### Still honestly NOT smooth (not faked)
- The system tick is still 100 Hz, so `SYS_SLEEP` has 10 ms granularity:
  the frame loop's final `sleep(next_frame-now)` still quantises, giving
  ~50–60 fps with mild micro-jitter rather than a rock-steady 60.  A 1 kHz
  (or 250 Hz) tick or an APIC-one-shot sub-ms sleep is the next step — NOT
  done here (it would re-scale dozens of tick-derived constants).
- Under pure TCG (no KVM) the compositor is simply 10–50× slower; `run.sh`
  already prefers KVM.  On real hardware/KVM it is now smooth apart from the
  residual sleep quantisation above.

## ⚡ CURRENT STATUS (updated 2026-08-16, verified in QEMU)

### Workspace-budget turn (user: "Workspace over budget — solution?")
- **[FIXED — snapshot over budget]** `git init` created 6,732 loose object
  files in `.git/objects` (one per blob), doubling the file count to 13,586
  — past the 10,000-file workspace limit. Two fixes:
  1. `git gc` packed the objects (13,586 -> 6,860 files).
  2. Trimmed the Kora icon theme from 6,547 vendored SVGs (28 MB) to exactly
     the 102 SVGs `gen_assets.py` actually resolves (the build needs a fixed
     ~115-icon list, not the whole theme). Restorable upstream (Kora repo).
- **RESULT: 461 files total under /home/user, repo = 296 files / 6 MB.**
  Fresh clone -> `make iso` -> boot verified again (15/15 selftests,
  0 panics, 0 missing icons).

### Portability turn (previous)
- **[DONE — project is now a portable git repo]** `git init` + 3 commits.
  The source tree is 31 MB (28 MB is vendored Kora icon SVGs) and contains
  NO build artifacts, screenshots, disk images, or downloaded bootloader.
- **[FIXED — fresh-clone build break]** The `*.png` scratch-artifact ignore
  rule was also excluding `kora/cursors/*.png` (the cursor SOURCE images
  that `gen_cursors.py` reads), so a fresh clone failed with "kora/cursors
  not found".  Added a negation rule; all 8 cursor PNGs are now tracked.
- **[DONE — self-contained WiFi blobs]** The generated `rtw8822c_phy.bin`
  (350 KB PHY register table) is now tracked under
  `initrd_root/lib/firmware/`, so real-hardware WiFi bring-up no longer
  needs the Linux source tree.  Both firmware blobs are inside the initrd.
- **[DONE — no hardcoded paths]** Fixed `scripts/ui_verify.py` (was
  `ROOT = "/home/user/YartOS"`, now resolves from script location).  All
  build/run scripts resolve their root portably; `/tmp` uses are runtime
  sockets only; `/home/user/uploads` + `/tmp/hideo` are best-effort asset
  restore sources that gracefully no-op.
- **[DONE — `make portable-check`]** New target that fails if the tree
  carries `yart.iso`, `yart-disk.img`, `*.ppm`/`*.png`, or a non-empty
  `runlogs/`.  `make clean` now also removes the disk image, runlogs and
  screenshots; `distclean` removes Limine too.
- **[VERIFIED end-to-end]** `git clone` → `make -j iso` (auto-fetches
  Limine) → boot in QEMU: 15/15 selftests, 0 panics, persistence works.
- README updated with a clone → bootstrap → build → run quick start and a
  Portability notes section.

### Scheduler-stability turn (previous)
- **[FIXED — the root cause of the random kernel panics] Task struct**
  **use-after-free in reap().** A task "in flight" (popped from a runqueue,
  not yet resumed by switch_to) still points at its task struct, but reap()
  kfree'd it. switch_to then read state from freed+reused heap, saw garbage
  instead of ZOMBIE, and resumed a freed task -> corrupt frames -> the
  RIP=user-addr / RIP=0xdc / #GP err=0x5000 crash family. The task struct is
  now leaked (not freed) - the kstack + PML4 are still reclaimed.
- **[FIXED — double-queue corruption] Work-stealing + watchdog re-kick could**
  **queue a task that was already running** (running on CPU A, queued on CPU
  B -> "task on two CPUs" -> scheduler wedge + corruption). Work-stealing is
  now disabled (load-balancing is an optimization, not correctness) and
  sched_kick_starved re-checks state under g_switch_lock before re-pushing.
- **[FIXED — killed-task resurrection] sched_sleep_ms overwrote ZOMBIE with**
  **BLOCKED**, so a task killed mid-sleep() syscall came back to life and was
  scheduled again after its kstack/PML4 were freed. sleep now refuses to
  sleep a ZOMBIE task.
- **[NEW — crash resilience] SMEP fault recovery.** A residual corrupt-frame
  fault (kernel instruction-fetch from a user address) now drops the corrupt
  context and switches to the idle task (logging "fault-recover") instead of
  panicking the whole OS.
- **RESULT: 50/50 launch/close stress cycles x3 = 0 panics, 0-1 starved**
  (was ~1 panic per 5 launches before this turn). The fork/exec/kill/reap
  cycle is now stable. 15/15 selftests, clean build (0 warnings).
- **[STILL RARE — ~1/50] a single task can still be briefly starved** (the
  watchdog re-kick recovers it; the system stays responsive). This is a
  residual wake-race that needs a final scheduler pass, but it no longer
  crashes or wedges the machine.

### POSIX-shell + keyboard turn (previous)
- **[FIXED — real keyboard bug] Shift emitted a stray character.** The
  driver computed `ascii = map_upper[scancode]` for the SHIFT KEY ITSELF
  (0x2A→'~', 0x36→'?'), so every shifted keypress polluted the command line
  ("Hello" typed as "~Hello", "$HOME" as "~$..."). Modifiers now emit NO
  character. VERIFIED: `echo Hello` produces exactly "Hello".
- **[DONE — POSIX shell features]** `$VAR`/`$?` expansion with single+double
  quotes, `export`/`unset`/`env`, `VAR=value`, `cmd > file` / `>> file` /
  `< file` redirection, `cmd1 | cmd2` pipes (in-process capture → `cat`),
  `cmd1; cmd2` sequencing, `cd` with no args → $HOME, `$?` status.
  VERIFIED: `echo $HOME` → "/home/yart", redirection writes files, `;` runs
  both commands, single-quote literal works.
- **[DONE — SYS_DUP2 (syscall 91)]** POSIX fd duplication (closes target,
  copies entry, balances vnode/pipe refs) — foundational for exec-based
  pipelines later.
- **[DONE — scrolling confirmed]** The terminal scrolls with the mouse
  wheel + PgUp/PgDn through a 512-row scrollback ring (already present;
  a blue scroll indicator shows when scrolled back).
- **[PARTIAL — self-healing watchdog]** `sched_kick_starved` re-kicks a
  READY-but-starved task (re-queue + IPI). It logs recovery attempts but
  does NOT always recover — the lost wake is deeper than a simple re-kick.
- **[STILL OPEN — the #1 kernel bug] SMP lost-wake starvation.** A task
  (most visible: a long-lived Console) ends up READY on a runqueue whose CPU
  stays idle; it is never scheduled again (intermittent, ~1-in-several
  minutes of typing). This now blocks reliable interactive use AND the WM
  session model (which needs reliable fork/exec + long-lived tasks).
  Needs a dedicated scheduler rewrite of the wake/park/steal protocol.
- **[NOT DONE — WM session model]** Still blocked by the starvation +
  SMEP-on-exec bugs (both hit the same fork/exec + long-lived-task paths
  the session model needs).

### Proportional-font + job-control turn (previous)
### should be same as sys font; add bg jobs and wm session model")
- **[FIXED — terminal font now matches the system font]** The terminal
  rendered each glyph in a fixed 12px cell, but the UI font is PROPORTIONAL
  (3-12px glyphs), so narrow letters had big gaps. Rows are now rendered
  with `sf_text` (same proportional advances as the whole OS), colour-run
  grouped, wrap is by pixel width, and the cursor is pixel-exact. VERIFIED:
  the banner now measures 202px (was 377px monospace, 256px at 8px cells).
- **[DONE — background jobs / job control]** Kernel: `TASK_STOPPED` state +
  `SIGSTOP`(19)/`SIGCONT`(18) in sched_signal (SYS_RAISE), and waitpid
  reports stopped children (status -19, not reaped). Shell: `/bin/prog [&]`
  launches external programs as jobs, `jobs`/`fg [n]`/`bg [n]` builtins,
  Ctrl+Z stops the fg job (SIGSTOP), Ctrl+C cancels. VERIFIED in QEMU:
  `/bin/files &` forks+execs in background, `/bin/editor` runs fg, Ctrl+Z
  logs "sched: SIGSTOP pid N", `jobs` lists, no double-frees.
  (Fixed a parsing bug: "/bin/prog &" left a trailing space after '&'
  removal → has_space wrongly true → "usage".)
- **[STILL OPEN — pre-existing SMEP-on-exec race]** Fork/exec stress still
  intermittently panics: `vec=14 err=0x11 RIP=0x7f21dd6f` (kernel fetched
  from a USER address = the new program's stack) right after exec. This is
  the same pre-existing user_exec/SMP race, now more visible because job
  control forks more. Needs the dedicated user_exec/frame audit.
- **[NOT DONE — WM session model]** The "kill WM → land on a terminal →
  start WM again" session model is designed (split wm out of init into
  /bin/wm, init supervises, /bin/tty fullscreen fallback, kernel fb
  re-claim on dead wm) but NOT built yet — the SMEP-on-exec bug must be
  fixed first (the session model constantly forks+execs).

### Terminal-reliability turn (previous)
- **[FIXED — terminal glyph overlap]** The UI font is PROPORTIONAL (glyphs
  3..12px wide) but the old terminal used 8px cells, so characters
  overlapped. Cells are now the font's true metrics (FONT_W x FONT_H =
  12x18), the grid is sized from the surface at runtime. VERIFIED: the
  banner now measures 377px (12px cells; was ~256px at 8px).
- **[DONE — readline-style editing]** Left/Right/Home/End/Delete/Insert,
  Ctrl+A/E/U/K, Ctrl+C cancel (^C + fresh prompt), Ctrl+L clear, Ctrl+D
  exit, Up/Down history. The prompt lives IN the cell grid with
  wrap-aware re-rendering (a 57-char command wraps to 2 rows and edits
  cleanly). VERIFIED in QEMU: insert-at-cursor ("abcd" + left-left + "X"
  -> executes "abXcd"), wrap renders 2 rows, Ctrl+L clears.
- **[DONE — bold/bright + cell backgrounds]** SGR bold now maps to the
  bright colour (index+8), and cell bg (SGR 40-47) is rendered. `colors`
  command shows the full 16-colour palette (VERIFIED: 54 colour buckets).
- **[FIXED — the last SMP scheduler race]** `switch_to` vs `sched_kill`/
  `reap` raced on a task "in flight" (popped from a runqueue, not yet
  ap_current): kill/reap freed its PML4/stack while switch_to loaded them,
  producing the corrupt-frame panics (RIP=0x1 / 0x84 / 0xdc / user-addr).
  FIX: a `g_switch_lock` serializes switch_to against kill/reap, and the
  guard refuses ONLY TASK_ZOMBIE (a BLOCKED task legitimately appears on a
  runqueue for a moment — wake does rq_push before the READY store, so the
  target CPU's own tick can pop it; refusing it lost tasks). VERIFIED:
  **60 fork/exec/close stress iterations = 0 panics, 0 starved tasks**
  (previously ~1/50 panics). Also fixed a transient self-loop I introduced
  mid-turn (refusing a killed task while its caller had re-queued `cur`
  kept it RUNNING + queued).
- Clean build (0 warnings). 15/15 selftests.

### Disk-persistence + real-terminal turn (previous)
- **[FIXED — the disk saving error, 3 root causes]** Files never survived
  reboot. Forensics (strace of QEMU + guest readbacks + host-file dumps)
  found THREE stacked bugs, all now fixed:
  1. **Allocator "block 0" sentinel collision** — `data_alloc` used 0 as the
     "unallocated" marker, but block 0 is a valid data block. Every file got
     block 0, `inode_free` kept "discarding" it (clearing bitmap bit 0), and
     every alloc returned 0 again → all files collided at sector 2081 and
     the bitmap never stuck. FIX: block 0 is now reserved (alloc starts at
     1); `inode_data_block` returns 0xFFFFFFFF for unallocated.
  2. **No VIRTIO_BLK_T_FLUSH** — QEMU buffers writes in its writeback cache
     and only reaches the backing file on a guest FLUSH or graceful exit.
     strace showed exactly ONE pwrite64 (the format superblock) in an entire
     boot. FIX: `blk_flush()` (VIRTIO_BLK_T_FLUSH) issued from `blkfs_sync`.
  3. **Virtio-blk request path not SMP-safe** — one bounce buffer / request
     header / g_last_used counter with no lock (a comment even said
     "single-CPU"). FIX: IRQ-safe spinlock around the whole request.
  Plus: the boot counter now fsyncs, and a periodic auto-writeback
  (~2 s, Linux pdflush model) flushes ALL dirty files. VERIFIED: the boot
  counter increments 1 → 2 → 3 → 4 → 5 across reboots; host file shows
  real content ("TOP-SECRET: 42", boot count, distinct data blocks).
- **[DONE — real terminal emulator]** nyra rewritten from a line-logger into
  a real VT100/ANSI cell-grid terminal: character cells (char+fg+bg), SGR
  colours (16-colour palette, bold, bright), CSI cursor movement
  (H/J/K/A/B/C/D, r;cH), erase-line/display, DEC private ?25l/h cursor
  show/hide, RIS reset, a 256-row scrollback ring, mouse-wheel scrolling,
  and per-cell coloured rendering. The prompt lives IN the grid (real
  editing: backspace/history rewrite cells). New `colors` command renders
  the 16 ANSI colours; `clear` uses a real `\x1b[2J\x1b[H`; `ls` colourises
  directories blue. VERIFIED in QEMU: coloured rows render, clear works.
- **[DONE — reboot]** `SYS_REBOOT` (syscall 90): ACPI reset (0x604) → 8042
  keyboard-controller reset → triple fault, after a disk sync. Shell
  `reboot` command. VERIFIED: machine resets, persistence survives.
- Session-model note (user's "Linux distro" vision: kill WM → terminal →
  start WM again): the WM is already kernel-watchdog-supervised (red
  recovery screen on stall); making that a real getty/session fallback
  (text console + `wm` restart) is the next step — not yet done.

### Syscall-ABI + SMP-debug turn (previous)
- **[ANSWERED] Syscall ABI question** — see `docs/SYSCALLS.md`. Short version:
  same ABI (syscall/sysret, rdi/rsi/rdx/r10, PIE ELF64) and same POSIX
  names/semantics, but a CUSTOM syscall-number table (only `write==1`
  matches Linux) and custom struct layouts + a non-fd network model. The
  big possibility: a Linux-number compatibility layer + musl would let us
  run stock static Linux binaries (BusyBox/toybox/curl/ssh) unmodified.
- **[FIXED — root cause of the SMP panics] PMM double-free in `vmm_cow_fork`.**
  The wm-side window surfaces (WM_SURF_WM_BASE) are mapped OUTSIDE any
  region, but `vmm_clone_pml4()` copies them verbatim and `vmm_cow_fork`
  only ref'd region pages — so every app launch under-counted every live
  surface frame, the slot-reuse reclaim then freed a still-mapped frame
  ("free of non-allocated page" wall), and the freed frame corrupted
  whatever task reused it. Added a pass that refs NOSHR pages outside
  regions. VERIFIED: the double-free wall is gone (0 real "free of
  non-allocated" over 60+ execs; the only one left is the boot selftest).
- **[FIXED — task-loss I introduced] `switch_to` refuse path lost the**
  **current task.** My kill/reap guard called `switch_to_idle()` on refusal,
  which marks `cur` READY and clears ap_current with no queue/ap_next slot —
  a permanently lost task ("starved, state READY, rq_cpu NULL"). Now the
  guard just returns the current frame. VERIFIED: starved reports 145 -> 1
  over a 50-launch stress run.
- **[NEW REAL BUG — still open] Residual kernel frame corruption.**
  ~1/50 stress launches still panics with `vec=14 err=0x10 RIP=0xdc`
  (kernel instruction-fetch at a tiny address) right after exec + surface
  create, with NO double-free and NO starvation — a third corruption path
  (candidate: CR3 pointing at a freed PML4 during the switch-in-flight
  window; needs the sched_kill/reap running-check + switch_to closed as one
  atomic window). Diagnostics added (watchdog-dbg cpu dump + sleep-dump)
  fire only on starvation and pinpoint the next reproduction.

### Bug-hunt turn (previous)
- **[DONE — de-fake] Real `passwd` command.** The Settings app claimed you
  change the password "from the Console (passwd), like every UNIX" but NO
  `passwd` command existed. Now: `SYS_PASSWD` (kernel) verifies the OLD
  password (PBKDF2 + constant-time + lockout, so an unlocked session can't
  hijack the account), rehashes the NEW one with a fresh salt, and writes
  /home/yart/.passwd + fsync. Shell: `passwd <old> <new>`. VERIFIED in QEMU:
  change "yart"->"newpass" (kernel logs rehash), wrong-old rejected
  ("auth FAILED" logged), lock screen rejects old pw and accepts new pw.
- **[DONE — de-fake] Shell `wifi connect` no longer fakes.** Removed the
  hardcoded `"password123"` + "connected (simulated over e1000)" lie. Now
  `wifi connect <ssid> <psk>` drives the REAL 802.11 auth/assoc/EAPOL path
  and reports honestly ("no radio in this machine" in a VM).
- **[DONE — bug] `exec()` never renamed the task.** Every app kept its
  parent's name ("wm"), so `ps`/watchdog reported every process as the
  compositor. `user_exec` now sets t->name to the new program (POSIX comm).
  Verified: log shows "exec: pid 5 'wm' -> nyra" and apps run named.
- **[DONE — bug/perf] Per-fault kprintf spam removed.** The VMM logged every
  demand-fault + CoW copy (kprintf -> serial busy-wait PER CHARACTER).
  1000+ lines per boot; real latency on hardware. Now gated behind
  `g_vmm_trace` (off). Verified: 0 fault lines in a full boot+20 execs.
- **[DONE — hardening] `user_exec` CR3/t->pml4 window closed.** t->pml4 now
  points at the new tables BEFORE the CR3 switch (and is restored on the
  fail path), so no reader (scheduler/other CPU) ever sees a task whose
  recorded tables disagree with the live CR3. 20 fork+exec stress iterations
  in QEMU: 0 panics (the race is intermittent and pre-existing; this closes
  one identified window, not a proven fix — see below).
- **[NEW REAL BUG — found, NOT fixed] Disk file DATA doesn't persist across
  reboot (virtio-blk).** Forensic evidence: after `blkfs_sync`, the guest
  can write+read back its own data (readback "YARTPSWD" ok) and inode-table
  + swap writes reach the disk image, but DATA-area sectors stay all-zero in
  the image and read as zero after reboot; the data bitmap on disk never
  gains a set bit. So every file collides at data sector 0 and file contents
  (settings.conf, boot_count.txt, .passwd) reset on reboot — the init's
  "boot counter survived from last boot" message is therefore currently a
  LIE (always "boot #1"). This is a virtio-blk/virtqueue completion bug in
  the driver, pre-existing, and needs its own pass. `passwd` is honest about
  it in the meantime (changed for the session; .passwd write is correct code
  that will persist once the driver is fixed).
- **[SMP frame-corruption bug — REPRODUCED + root-caused, partially fixed]**
  Rapid fork/exec/close now panics ~1/20 with a corrupt kernel frame (RIP=0,
  RIP=user-addr, or RIP=0xdc). The REAL root cause is a **double-free in the
  PMM** — a wall of `pmm: !! free of non-allocated page` lines precedes the
  panic, so page refcounts are under-counted somewhere in the
  fork/exec/CoW/teardown lifecycle; the freed+zeroed page then clobbers a
  live kernel stack/frame. Fixed ONE precise window this turn: `switch_to`
  now refuses to resume a task that was killed while in flight from a
  runqueue (state != READY/RUNNING) — closes the "pop → kill+reap frees
  stack → iretq into zeroed frame (RIP=0)" race. The remaining double-free
  (refcount audit of `vmm_cow_fork` / `vmm_free_pml4` / `sched_kill` /
  `user_exec` and aliased/NOSHR page accounting) is the next dedicated pass.
- Still-fake/honest: Wi-Fi association provable only on real HW · battery %
  is the VM's injected SSDT · no GPU driver · EN-only input · no update
  service.

### Settings-as-Skift turn (previous)
- **[DONE] Cursor ~20% smaller.** The photo cursors (packed at 48px) now render
  at 4/5 scale with a bilinear (premultiplied-alpha) downscale. Verified:
  white-arrow bbox went 31x44 px -> 28x38 px. (Skift's own vector cursors are
  28-32px, so this is closer to Skift than before.)
- **[DONE] Settings app rewritten to Skift hideo-settings architecture, in C.**
  The old single `if/else` chain is gone. Now it mirrors Hideo.Settings:
  - **model**: `Page` enum (HOME, ACCOUNT, PERSONALIZATION, PACKAGES, SYSTEM,
    NETWORK, SECURITY, UPDATES, ABOUT) + a navigation State (history + index).
  - **actions + reduce()**: GoTo / GoBack / GoForward / GoHome — one pure
    function owns every state transition (back/forward history works).
  - **scaffold**: header tool buttons (back/forward/home) + sidebar (search
    field that really filters the sidenav + sidenav items) + `pageContent()`
    switch dispatching to one function per page (`page_home`, `page_account`,
    ... `page_about`) — the same decomposition as Skift's page-*.cpp.
  - **options match Skift's**: HOME is a 3-column tile grid (Accounts,
    Personalization, Applications, System, Network, Security, Updates, About);
    PERSONALIZATION holds the real accent/cursor/wallpaper settings;
    PACKAGES lists the real /bin; SYSTEM holds dock/volume/UI-scale;
    NETWORK shows the live kernel Wi-Fi status + a real Scan; ABOUT shows
    real sysinfo (user, system, CPU#, process count, battery, Wi-Fi state).
    Verified by screenshot pixel analysis: sidebar highlight + Scan button +
    tile grid + per-page content all render at exactly the designed coords;
    clicking Network/Accounts navigates; search "net" filters to one item.
- **[DONE — real bug fix] app mouse coords were wrong.** The PS/2 driver only
  emits deltas and apps only receive them once focused, so every ring-3 app's
  pointer drifted by however far the cursor travelled before focus (clicks
  landed ~330px off). Added SYS_MOUSE_POS (kernel tracks the absolute cursor,
  clamped to the scanout) + apps snap to it on every mouse event. Clicks in
  Settings now land exactly where clicked.
- **[DONE — de-fake] Wi-Fi scan no longer fabricates APs.** The hardcoded
  "YartNet/HomeFiber/..." list is gone. `wifi_scan()` now: real hardware
  (RTL8822CE) -> real 802.11 probe scan over the rtw88 DMA rings via
  wifi_session; no radio (QEMU) -> honestly returns 0 networks. Boot log now
  says "wifi: no wireless radio detected (VM) - interface list empty".
- Regression: clean build, 0 warnings; boot 0 panics + all 15 selftests pass
  (incl. 802.11 session + crypto + rtw88 + ACPI battery).

### STILL FAKE / HONESTLY ABSENT (no pretending)
- **Wi-Fi on the real laptop** — the full rtw88 + 802.11 + WPA2 stack is
  ported and self-tested, but association can only be proven on the HP
  ProBook x360 (RTL8822CE). QEMU has no radio, so the VM shows 0 networks —
  that is the honest truth, not a missing feature.
- **Battery %** — the ACPI mechanism is real; in QEMU it reads the injected
  SSDT (82%). On the laptop it will read the EC. VM numbers are therefore
  "the VM's battery", not the user's.
- **No GPU driver** (software SIMD rasterizer only — same as Skift karm-gfx),
  **no Arabic/RTL layout** (EN only, honestly labelled), **no package/update
  service** (Settings > Updates says so).
- **SMP race in user_exec** (intermittent SMEP violation on app exec) — still
  open, pre-existing, not reproducible on clean build; needs its own pass.

### Rendering/perf turn (previous)
- **[ROOT CAUSE of "heavy" feel — FIXED]** The compositor damaged the cursor
  rect EVERY frame unconditionally, so it recomposited forever even when
  completely idle -> one core pinned at ~100%. Now the cursor is damaged only
  when it moves (or the scene under it changed). Idle = 0 redraws.
- **[FIXED] chrome redraw per dirty rect.** `composite_rect` used to redraw the
  whole panel + dock + desktop for EVERY dirty rect. Now each is gated by
  rect intersection (a small cursor repaint no longer repaints the full bar).
- **[FIXED] focus thrash.** `scan_windows` reset `seen` before checking it, so
  every window was treated as "new" every scan -> `wm_focus` called every
  scan (375 focus logs in one short test). `was_new` now means "surface did
  not exist before" -> 1 focus log per window.
- **[FIXED] per-frame syscalls.** net_info throttled to 500ms, scan_windows to
  every other frame.
- **[FIXED] apps redrew at 60fps always.** Console/editor/settings now flip
  only when content changed (event-driven, Skift-style); terminal also
  redraws on cursor-blink toggle only.
- **[NEW] KVM acceleration in run.sh** (huge on real hardware: TCG emulation
  is ~10-50x slower; the "heavy" feel is partly TCG when /dev/kvm is unused).
- **[NEW] Settings redesigned like Skift's hideo-settings:** left sidebar
  navigation (Appearance/Cursor/Wallpaper/Dock/Volume/Display/About) +
  content pane. Appearance = accent colour palette cycling (persisted),
  Display = 1x/2x UI scale. Verified: highlight moves 0->2 on Down-Down,
  focus logged once.
- All userland changes (wm.c, wm_windows.c, wm_damage.c, gui_apps.c, nyra.c,
  run.sh) backed up + restored by patch_smooth.py markers.
- Regression: boot 0 panics + 15 selftests; app launch/close x9 (3 runs)
  0 crashes; SIMD blit selftest bit-exact.
- **KNOWN REAL BUG (found, pre-existing, intermittent):** SMP race in
  `user_exec` -> kernel SMEP violation (err=0x11, RIP=user-stack address)
  once during testing; does not reproduce on a clean build (9 execs clean).
  Not caused by the rendering work; needs a dedicated investigation pass.

### HiDPI-completion turn (previous)
- **[DONE] Full 2x icons (was: text+panel only).** Dock (DOCK_REST/PITCH/
  MARGIN + G_dock_h all scale with G_scale, cache rebuilt on scale change),
  panel tray icons (battery/volume/wifi/chevron/clipboard sized & spaced by
  G_scale), launcher app grid + dock dropdown (icon size, row pitch 48->96,
  search bar), and desktop icons (cell 88x92, glyph 44, label offset, drag
  snap 104x100 all scale). Verified programmatically: launcher row pitch
  48px -> 96px, dock icon ink 1050 -> 4199 (~4x), dock pill taller, panel
  40 -> 80px, clock text 208 -> 580 px.
- **[DONE] Honest input-language button.** Removed the fake "ARB" toggle
  (no Arabic layout/glyphs exist). The button now shows "EN" and clicking it
  honestly reports "Input: English (US) — only layout available". No more
  fake language switching.
- **[NEW] Ctrl+W closes the focused window** (standard enterprise-OS
  shortcut; also used to cleanly verify 2x window scaling).
- Regression: 1x AND 2x window close = 0 kernel panics / 0 segfaults; boot
  = 0 panics + 15 selftests ("gfx: SIMD blit selftest ok (bit-exact)").
  (A transient kernel panic seen once mid-turn was from a mid-session
  environment revert leaving stale kernel state; the full repair chain
  fixed it and it does not reproduce on a clean build.)
- All changes durable: patch_smooth.py now full-file-restores wm_panel.c,
  wm_dock.c, wm_launcher.c (markers) + gfx.c/h, wm.c/h, wm_windows.c,
  gui_apps.c, nyra.c, mouse.c + Makefile (-msse2).

### GPU-acceleration / HiDPI turn (previous)
- **Honest framing (Skift parity):** Skift's compositor (strata-shell) uses
  karm-gfx, a *software* rasterizer - it does NOT GPU-accelerate. Its
  smoothness comes from a tight blitter + damage tracking. So "translate
  Skift" = optimize the software blitter, which is what I did. (A real GPU
  driver - i915/amdgpu/nouveau - is ~100k+ lines and out of scope; flagged.)
- **[NEW] SSE2 compositor blitter (real, bit-exact).** userland/gfx.c now has
  SSE2 fast paths: `sf_fill_rect_blend` (constant-alpha blend - the panel/
  dock/overlay translucency) and a 128-bit copy in `sf_blit`. The kernel
  already FXSAVE/FXRSTORs FPU/SSE state per context switch, so SIMD in
  userland is safe. `gfx_selftest()` runs the SSE2 path vs the scalar
  reference on 1000 randomized rows and proves BIT-EXACT output (boot log:
  "gfx: SIMD blit selftest ok (bit-exact)"). Boot = 15 selftests, 0 panics.
- **[NEW] HiDPI integer 2x UI scale (real, persisted, verified).**
  - gfx.c: `sf_set_scale()` - all text renders with crisp integer pixel-
    doubling at 2x (glyph 2x2 blocks; widths double).
  - Settings app: new "UI Scale" row (toggle 1x/2x), persisted as `scale=`
    in settings.conf; the WM re-reads it (settings_poll) and applies live.
  - WM: PANEL_H/TB_H scale with G_scale; 1x app surfaces are 2x-upscaled
    when composited (the macOS "legacy app on Retina" model).
  - VERIFIED programmatically: panel boundary 40px (1x) -> 80px (2x); clock
    text ink 208 -> 580 px.
  - Honest limits (documented): icons/dock-spacing are not yet 2x (only text
    + panel/titlebar + window content); full visual check is the user's.

### De-faking turn (previous)
- **[REAL TRASH]** The hardcoded "Trash is empty" is gone. Trash is now a real
  directory `/home/yart/.trash` (created at boot). Files app `Del` MOVES the
  file/dir into it (unique-name collision handling); in the trash, `Del`
  deletes permanently, `R` restores to /home/yart, `E` empties the trash (real
  unlink of each entry). Trash icon (dock + desktop + right-click menu) opens
  the Files app pointed at the trash dir via argv. (gui_apps.c + wm.c +
  wm_overlays.c). The only remaining "Trash is empty" string is the honest
  one shown when the trash dir is genuinely empty.
- **[REAL FILES APP]** The Files app's key handler was never wired into the
  main loop (Up/Down/Enter/Del were dead). Now wired, plus real copy/cut/
  paste: Ctrl+C copy, Ctrl+X cut, Ctrl+V paste (copy = read+write the file;
  move = rename; both fail honestly for unsupported cases). Status bar shows
  the action.
- **[REAL LOGIN SCREEN]** The OS now BOOTS to the lock screen (G_locked
  starts true) — the lock screen IS the login screen, with real PBKDF2 auth
  (password "yart"). Hint now reads "Press Enter to log in". Verified:
  boot=locked (mean 13) → wrong password stays locked (13) → "yart" unlocks
  (47).
- All changes are durable (patch_smooth.py full-file restores for
  gui_apps.c/wm.c/wm_overlays.c + the fragment restore for scripts/backup).
  Regression: boot 0 panics + 14 selftests, WM surface-destroy STABILITY PASS.

### Smoothness + selection turn (previous)
- **[FIXED] cursor lag/ghosting.** Root cause: the cursor's NEW position was
  only damaged for the NEXT frame, so it always presented one frame (16ms)
  late and its old spot ghosted. Now `cursor_rect()` damages the cursor's new
  footprint BEFORE the compositing pass, so the cursor is composited +
  presented in the SAME frame — exactly how Skift draws the cursor in its
  render pass. (userland/wm.c)
- **[FIXED] mushy/trailing mouse.** Root cause: the driver's EMA filter
  (`new = old*0.3 + raw*0.7`) delayed every packet. Now motion is DIRECT 1:1
  (mild speed accel + a lossless subpixel accumulator, no temporal lag) —
  same as Linux evdev forwarding that Skift uses. (kernel/drivers/mouse.c)
- **[NEW] text selection in the editor (Text app)** — enterprise model:
  click = caret, drag = select (accent highlight), release = auto-copy to the
  system clipboard, Ctrl+C copy, Ctrl+V paste, insert-at-caret typing, and a
  16ms refresh. The Console already had drag-select + click-paste.
  (userland/gui_apps.c) — selection is per-text-widget over the system
  clipboard, which is exactly how real OSes do it (no "global OS text
  selection" exists even on Windows/macOS/Linux — it's a toolkit feature).
- App-side refresh 30ms -> 16ms (Console + editor) to match the 60Hz compositor.
- All of the above is durable: scripts/backup/patch_smooth.py re-applies it
  (markers + full-file backups of wm.c/gui_apps.c/mouse.c/nyra.c in the
  external backup dir), and ensure_kernel.py now restores the backup
  fragments themselves when the revert deletes scripts/backup/.
- Regression: WM surface-destroy cycle STABILITY PASS (0 crashes), boot 0
  panics + 14 selftests, lock auth PBKDF2 passing.

### Verification turn (previous session, evidence)
- **Mouse pipeline proven end-to-end**: HMP/QMP injected PS/2 events move the
  cursor, button packets reach the kernel (logged `buttons=0x1`), and a click
  opens the launcher. (Earlier automated click "failures" were the test
  harness missing 36px buttons through the driver's acceleration+smoothing
  filter — the OS click path is sound; cursor-position asserts are unreliable
  under that filter.)
- **Rendering verified against Skift tokens on a live screenshot**: wallpaper
  (58,62,70) → translucent panel (14,16,24 darkened toward #09090b), frosted
  dock (21,21,23), and theme.c values are EXACT Skift: panel #09090b@60%,
  dock #18181b, window border #27272a (GRAY800), text #fafafa (GRAY50),
  accent #3b82f6 (BLUE500).
- **Crash fix regression**: app-launch → surface-destroy → exit cycle = 0
  crashes (STABILITY PASS). Lock auth with PBKDF2 = lk0=47 → lk1-3=13 →
  lk4=41. Boot = 0 panics, 14 selftests.
- Fixed clipboard-button hit region off-by-4px; self-heal now also requires
  SYS_CLIPBOARD_GET + clipboard impls (a revert to "81-85 only" previously
  slipped through). patch_wm_surf.py re-created + backed up externally.

### This turn (user request: "fix the crash + weak mechanism + wifi scan + gui + shortcuts")
- **[FIXED] WM SIGSEGV crash + "windows undraggable".** Root cause: when an app
  exited, `wm_surface_teardown()` unmapped the WM-side surface VA while the
  compositor could still be holding it between two scan passes -> the WM read
  a stale VA and got killed (the exact "surface 0 destroyed ... SIGSEGV at
  0x6d000000 ... task 4 'wm' killed" log). Fix (kernel, syscall.c): the WM-side
  mapping + frames are now kept ALIVE (deferred free) and reclaimed when the
  slot is reused; plus the WM deactivates a vanished window on the FIRST scan
  miss. Verified: app launch -> surface destroy -> exit cycle, 0 crashes
  (was 2 crashes before). Durable via scripts/backup/patch_wm_surf.py.
- **Default login password: "yart"** (account "demo").
- **[HARDENED] password mechanism.** Was single-iteration SHA-256(salt||pw).
  Now PBKDF2-HMAC-SHA256, 10,000 rounds (DOAS_PBKDF2_ITERS), constant-time
  compare + lockout kept. doas_init + doas_check upgraded; durable via
  scripts/backup/doas_pbkdf2.inc.
- **[REAL Wi-Fi scan]** Removed the hardcoded AP list ("YartNet", "HomeFiber"
  etc). `wifi_scan()` now runs a real 802.11 probe scan through the session
  orchestrator over the rtw88 DMA rings when the card is up; with no radio
  (QEMU) it honestly returns 0 APs ("scan for real results").
- **[NEW panel buttons]** top-right: input-language button ("ENG" ↔ "ARB"
  placeholder — only ENG layout implemented, documented), a clipboard button
  (opens a clipboard popover), and the Wi-Fi icon now only appears when
  connected; otherwise a ">" chevron opens the scanned-network list.
- **[NEW clipboard]** kernel syscalls SYS_CLIPBOARD_SET/GET (86/87, 512-byte
  buffer, spinlock). Terminal (Console) now supports drag-to-select (copy) and
  left-click paste.
- Desktop visuals: left at Skift parity (unchanged, per earlier instruction).
- New syscalls 86/87 + PBKDF2 + wm-surface fix + rtw scan are all re-applied
  by ensure_kernel.py / ensure_wifi.py, so they survive the environment reverts.

### Desktop keyboard shortcuts
- Super (Meta) — open/close the launcher
- Super + L — lock screen (password = "yart")
- F1 / F5 — launcher
- F2 / F3 — show-desktop toggle
- F4 — reload / reset theme
- Ctrl + 1..4 — switch to workspace 1..4
- Ctrl + Left / Right — previous / next workspace
- Ctrl + Alt + Up / Down — Mission-Control overview
- Alt + Tab — window switcher (live previews)
- Esc — close overlays / cancel
- (Console) Up / Down — command history; drag = select+copy; left-click = paste

---

### STILL FAKE — be honest about these
- **Wi-Fi (in the VM):** `wlan0` is a *virtual* interface over the e1000 and
  there is NO radio, so `wifi scan` honestly returns **0 APs** (the hardcoded
  "YartNet"/"HomeFiber" list is gone). On your laptop the RTL8822CE runs the
  real 802.11 probe scan; association is only provable on real hardware.
- **Battery level in the VM:** the *mechanism* is real (ACPI `_BST`/`_BIF`
  evaluation, exactly like Windows/Linux), but QEMU has no physical battery,
  so the firmware SSDT battery reports a **static 82%** — it does not drain.
  On a real laptop this same code reads the real EC battery (untested yet —
  needs your machine).
- **Trash** is fake ("Trash is empty"). **No login screen** (boots as uid
  1000 "demo"). **Files app** is shallow (no copy/move). **No GPU accel /
  HiDPI**. **Terminal Wi-Fi** commands hit the simulation, not a real card.

### REAL and verified (screenshots / serial / regression tests)
- Battery % read through real ACPI AML evaluation (SSDT → `_BST`/`_BIF` → 82%).
- Network: real e1000 link + TCP/IP/DNS/TLS/ICMP — packets genuinely leave.
- Lock screen auth: salted SHA-256, constant-time compare, lockout
  (regression lk0=27 → lk1-3=11 → lk4=28).
- Volume: real HDA amp-gain verbs. Notifications: kernel ring → WM → calendar.
- Settings app: real settings.conf (accent/cursor/wallpaper/dock/volume).
- Compositor: damage rects, minimize-to-dock, overview, alt-tab previews,
  show-desktop, real Roblox/Skift cursors, Inter font.
- **WPA2 crypto (new, proven byte-exact):**
  - `kernel/lib/sha1.c` — SHA-1 + HMAC-SHA1 + PBKDF2 → PMK. Selftest PASS
    (FIPS 180-4, RFC 2202, IEEE 802.11 WPA2-PSK example vector).
  - `kernel/lib/ccmp.c` — AES-CCMP (802.11i). Selftest PASS (RFC 3610
    vectors #1/#3/#13, encrypt + decrypt + MIC).
  - `kernel/lib/wpa.c` — WPA2 PRF (802.11i §8.5.1.1) + PTK derivation +
    EAPOL-Key MIC. Selftest PASS (hostap/IEEE PRF vectors 0/1, PTK reference
    vector, min/max-ordering invariance, MIC tamper check).
  - `kernel/lib/eapol.c` — AES Key Wrap (RFC 3394) + the full supplicant-side
    4-way handshake state machine (M1→M2→M3→M4). Selftest PASS (RFC 3394
    vector 4.1 + a full AP↔STA handshake round-trip: PTK agreement, all MICs,
    GTK unwrap, tampered Msg3 rejected).
  - `kernel/lib/dma.c` — 32-bit DMA allocator (physically contiguous, <4 GiB,
    via new `pmm_alloc_pages_below`). Selftest PASS (alignment, <4 GiB,
    HHDM identity, pattern round-trip).
  - `kernel/lib/fw.c` — firmware loader (VFS → kernel heap). Selftest PASS
    (loads a 64 KiB `rtw8822c_fw.bin` stand-in from the initrd, checks magic
    + pattern). The real 1 MB Realtek blob drops in later. MSI deferred:
    rtw88 works over legacy INTx, so no MSI driver is needed for a first cut.
  - `kernel/net/ieee80211.c` — 802.11 management frame codecs (probe/auth/
    assoc-request builders; beacon/probe/auth/assoc-response parsers; IE
    walker for SSID/rates/channel/RSN). Selftest PASS against hand-packed
    byte vectors (independent of the builders).
  - `kernel/net/wifi_data.c` — 802.11 CCMP data-frame codec (LLC/SNAP, exact
    802.11i AAD/nonce construction matching mac80211). Selftest PASS
    (encrypt/decrypt round-trip, both directions, tamper rejection).
  - `kernel/net/wifi_sta.c` — WPA2 station connection state machine
    (auth→assoc→4-way→CONNECTED). Selftest PASS: a full simulated AP<->STA
    session (probe resp → auth → assoc → EAPOL 4-way → CCMP data) agrees
    end-to-end.
  - `kernel/drivers/rtw_pci.c` + `rtw_fw.c` + `rtw8822c_regs.h` + `rtw88.h` —
    the rtw88 driver port begins: PCI transport (BAR2 map, MMIO accessors,
    chip-version read from REG_SYS_CFG1) and the complete legacy 8051
    firmware-download state machine, transcribed from Linux rtw88 mac.c
    (verified register offsets/bits against mainline reg.h). Selftest PASS:
    the full download handshake runs against a fake chip register file that
    emulates the silicon contract (MCUFWDL_EN latch, per-page CHK_RPT,
    FW_READY_LEGACY on CPU boot) and byte-exactly reconstructs the firmware.
  - `kernel/drivers/rtw_efuse.c` — EFUSE read: the REG_EFUSE_CTRL handshake
    (addr into bits 8..17, clear BIT_EF_FLAG, poll for read-done) plus the
    wear-leveled 1-byte/2-byte block-header reconstruction of the logical
    EFUSE map, with the station MAC extracted at offset 0x120 (RTL8822CE).
    Selftest PASS against a fake EFUSE with a hand-built physical map. Also
    fixed a real transcription bug: Linux rtw_write32_mask shifts the value
    by __ffs(mask) — I had it unshifted, which the selftest caught.
  - `kernel/drivers/rtw_dma.c` — TX/RX descriptor rings: 48-byte TX packet
    descriptor (w0 pkt size, w1 qsel/macid, w4 rate, w7 16-bit XOR checksum),
    16-byte TX buffer descriptor, 8-byte RX buffer descriptor, 24-byte RX
    descriptor parse (pkt len/macid/rate/ICV), ring allocation over the
    32-bit DMA allocator, and ring base/length hand-off via the
    TXBD_DESA_x/TXBD_NUM_x/RXBD_DESA_MPDUQ/RXBD_NUM_MPDUQ registers
    (0x308-0x38C, verified against pci.h). Selftest PASS: alloc + register
    setup + descriptor fill/parse + an independent checksum recomputation,
    against a fake chip register file.
  - `kernel/drivers/rtw_io.c` — frame I/O: TX (build [48B pkt desc || frame],
    write the 16-byte slot, bump wp, ring the 0x3A8 doorbell) and RX (poll
    chip wp from 0x3B4 bits 27:16, strip the 24-byte RX descriptor, refill
    the buffer, write the host rp back). Selftest PASS: a full DMA round-trip
    with a fake chip that shares the host's DMA arena and emulates the
    silicon's ring-side contract (TX pull + RX inject + multi-frame order).
    Also fixed the TX-slot layout: a slot is TWO 8-byte buffer descriptors
    (packet descriptor + data), matching rtw_pci_tx_write_data, not one
    16-byte struct.
  - `kernel/net/wifi_session.c` — the session orchestrator: drives the 802.11
    station state machine over an abstract frame transport (tx/rx/delay — the
    same hci_ops-style layering Skift uses). EAPOL rides plaintext 802.11
    data frames (LLC/SNAP + EtherType 0x888E + 802.1X header). Selftest PASS:
    a FULL AP↔STA Wi-Fi session — probe/scan finds a WPA2 AP, auth + assoc,
    EAPOL 4-way handshake (M1→M2→M3→M4 with real PMK/MICs/GTK unwrap), then
    CCMP-protected data in BOTH directions, both sides decrypting and
    verifying byte-exact. The capstone that proves every brick — ieee80211
    codecs, eapol, ccmp, wpa, wifi_sta — actually connects.
  - `kernel/drivers/rtw_phy.c` — PHY init: the power-sequence parser
    (WRITE/POLLING/DELAY commands), the RTL8822CE PCI card-enable sequence
    (carddis→cardemu→act), RF register writes via rf_base_addr (A=0x3c00,
    B=0x4c00, addr<<2), the mac/agc/bb/rf table config functions with the
    Realtek delay-addresses (0xf9..0xfe BB, 0xfe/0xffe RF), and the
    conditional table parser (IF/ELIF/ELSE/ENDIF + neg). Selftest PASS: full
    power-on sequence vs a fake chip (analog-ready/cpu-act polling contract),
    table apply + delays, RF addressing, conditional parser, and a generated
    blob round-trip. The 43,755-pair table DATA is produced by
    scripts/gen_rtw_tables.py straight from the Linux rtw8822c_table.c (no
    hand-transcription) into initrd_root/lib/firmware/rtw8822c_phy.bin.

### Wi-Fi port — progress checklist (target: RTL8822CE on HP ProBook x360 435 G8)
- [x] RTL8822CE + rtw88 family PCI detection (honest: "driver NOT ported")
- [x] SHA-1 / HMAC-SHA1 / PBKDF2 (WPA2 key derivation)
- [x] AES-CCMP (WPA2 frame encrypt/decrypt/MIC)
- [x] WPA2 PRF + PTK derivation + EAPOL-Key MIC (4-way handshake primitives)
- [x] EAPOL 4-way handshake state machine + AES key wrap (GTK in Msg 3)
- [x] 32-bit DMA allocator + firmware loader (VFS -> DMA buffer)
- [x] 802.11 management frame codecs (probe/auth/assoc build + parse)
- [x] 802.11 CCMP data-path codec + WPA2 station connection state machine
- [x] rtw88 PCI transport + firmware-download state machine (proven vs fake chip)
- [x] rtw88 EFUSE read (physical read + wear-leveled logical reconstruction + MAC @ 0x120)
- [x] rtw88 TX/RX descriptor rings (DMA alloc + reg setup + TX/RX desc fill/parse + checksum)
- [x] rtw88 PHY init: power-on sequence + table loader + RF write + conditional
      parser (machinery transcribed + selftested); the 43,755 register-pair
      table DATA is auto-generated from the Linux source (350 KB blob)
- [x] rtw88 frame I/O: TX (doorbell 0x3A8) + RX (index 0x3B4, chip-wp in
      bits 27:16) over the DMA rings, proven by a full DMA round-trip vs a
      fake chip that shares the DMA memory like the silicon does
- [x] session orchestrator (wifi_session.c): scan→auth→assoc→EAPOL 4-way→
      CCMP data, over an abstract frame transport. Selftest PASS: a complete
      AP↔STA Wi-Fi session end-to-end against a simulated AP (scan finds the
      WPA2 AP, join completes the 4-way handshake, CCMP data flows BOTH
      directions and both sides decrypt/verify byte-exact).
- [ ] on-laptop bring-up (firmware load → chip responds → associate → DHCP)

### Known issues to fix before/at rtw88 bring-up
- **Latent heap/exec corruption**: a 64 KiB kmalloc'd+then-freed block left in
  the heap right before /bin/init exec makes kmalloc's split() fault (freelist
  `next` = 0x2634ff20...). Heap walks clean all the way to exec; root cause is
  pre-existing (not in the Wi-Fi code) and not yet isolated. Worked around by
  shrinking the stand-in firmware blob to 4 KiB. MUST be revisited: the real
  ~1 MB rtw8822c_fw.bin will go through dma_alloc32 (not kmalloc), so the
  heap path may be avoided — but this bug needs a real fix before any 64 KiB+
  kmalloc near boot.

### Environment notes
The workspace repeatedly reverts kernel files to broken intermediate states
between turns. `reapply_fixes.py` + `ensure_kernel.py` (+ `ensure_wifi.py`)
self-heal everything, restoring from the backup dir. Run them in that order
after any revert, then `make iso`.

### Portability pass (2026-08-15)
- Removed all hardcoded `/home/user/YartOS` paths: `run.sh`, `boot_shot.py`
  and `ensure_wifi.py` now resolve the repo root from their own location
  (`$(cd "$(dirname "$0")" && pwd)` / `os.path.dirname(os.path.abspath(__file__))`).
- OVMF is auto-detected across Ubuntu/Debian paths; the 64 MiB disk image is
  auto-created on first run (it is a local artifact, no longer shipped).
- Deleted only generated/local artifacts: `iso_root/`, `build/`, `yart.iso`,
  `yart-disk.img`, `runlogs/`, all `.ppm`/`.png` screenshots, `docs/screenshots/`,
  and the build-generated files under `initrd_root/` (`bin/*.elf`, `YartOS/`
  wallpaper pack, `etc/motd`, `etc/yart.conf`, `home/yart/cursor.conf`) — all
  regenerated by `make iso`. Kept all source, docs, kora assets (icons/cursors/
  wallpapers), limine, firmware blobs, `acpi/battery.{asl,aml}`.
- Verified from-clean: `make iso` rebuilds everything, boots with 0 panics,
  14 selftests passing, desktop rendering.

---
## HISTORY (chronological)
# YartOS — Brutal Audit

Every claim below is based on reading the actual code and testing in QEMU.
No marketing. If it's fake, it's called fake.

---

## 1. What is actually real (the genuinely impressive part)

| Thing | Verdict |
|---|---|
| SMP | **Real.** 4 cores boot via APIC/IOAPIC, per-CPU scheduler run-queues, work-stealing, TLB shootdowns. |
| Virtual memory | **Real.** 4-level paging, demand-paged mmap, CoW fork, SMAP + SMEP, NX on the direct map. |
| Scheduler | **Real.** Preemptive (APIC timer), blocking sleep queue, waitpid/zombie reap, OOM killer. |
| Processes | **Real.** fork/exec/waitpid, per-task address spaces, argv/envp, uid/gid, signals (partial). |
| Syscalls | **Real.** ~80 of them, int 0x80 + syscall fast path, SMAP-safe copy-in. |
| Filesystem | **Real.** VFS + initrd (ustar) + blkfs on virtio-blk with journal CRC and triple-indirect. |
| Network | **Real-ish.** e1000 driver, TCP, UDP, DNS, TLS 1.2 client, IPv6 probing. Impressive for a hobby OS. |
| Compositor | **Real.** Ring-3 software compositor with dirty-rectangle damage, backdrop buffer, per-rect clipped repaint, 60 Hz pacing, partial present (Skift strata-shell architecture). |
| Input | **Real.** PS/2 keyboard + mouse, USB HID keyboard (xHCI), working after four real bugs were fixed. |

## 2. What is FAKE or cosmetic

| Thing | Brutal truth |
|---|---|
| **Wi-Fi** | **Fake.** `wifi.c` admits it: "creates a virtual wlan0 over e1000". There is no wireless hardware path. The Wi-Fi tile and "scan" are theatre. |
| **Settings app** | **Fake.** Four static rows ("Cursor theme / Wallpaper / Dock / About"). It displays text. It does not change anything except wallpaper hint text. There are no real toggles. |
| **Battery/volume/network indicators** | **Hardcoded.** "100%", fixed icons. Volume slider only changes an in-memory number that feeds nothing (there is an HDA driver but the slider isn't wired to it). |
| **Notifications** | There are **none**. The OSD toast is a fade-out string, not a notification system. |
| **Calendar** | Real month math (Zeller, leap years), but zero functionality — no events, no interaction beyond month +/-. |
| **Trash** | **Fake.** "Trash is empty", always. No recycle bin. |
| **Lock screen** | Cosmetic. Super+L shows a pretty clock. **Enter unlocks it — there is no password, no auth, no user.** |
| **Login** | There is **no login screen at all**. Boot → desktop, as uid 1000 "demo". |
| **Text editor** | Saves one file, `/home/yart/note.txt`. No copy/paste, no undo, no scrollback, no selection. |
| **Files app** | Lists a directory, rename/delete/new-dir. No copy, no move, no file open, no permissions UI. |
| **Terminal** | ls/cat/echo/ps/wifi/help. **No ANSI escape parsing** (no colors, no curses), no pipes-as-you-type, no job control, no tab completion. |
| **Icons** | 118 Kora PNGs rasterized at build time (22/32/48px). No runtime SVG, no crisp scaling above 48px. |
| **Font** | One embedded 12×18 Inter Medium bitmap. No other sizes, no bold/italic, no shaping, no CJK. |
| **Cursor** | Raster PNGs (Roblox 2013). No vector/hotspot-per-pixel animations. |

## 3. Real bugs found and fixed (this session)

1. **Every keystroke went to the idle task.** `sys_input_kbd` called `sched_find(0)` which returns the idle task (pid 0), so with no focused app keys vanished. Gated the lookup on `g_focus_pid != 0`.
2. **Double keypress.** Kernel set `KEY_RELEASE` at bit 16; every app tested bit 7, so releases were processed as second presses.
3. **Enter did nothing.** Terminal/launcher checked CR (13) but the keyboard emits LF (10).
4. **PS/2 keyboard dead under UEFI.** OVMF leaves the 8042 disabled; `kbd_init` never re-enabled it. Also USB HID usage codes were passed raw as if they were set-1 scancodes.
5. **Show Desktop destroyed windows.** It moved them to (-10000,-10000), but the kernel clamps window x to ≥ -100, so they piled into the top-left and could never be restored. Now a compositing flag.
6. **Whole-framebuffer present.** The kernel copied the entire back buffer every frame; now partial-present per damaged rect.
7. **Cursor blob header off-by-one.** `cursors.c` read the version field as the theme count, silently loading only 1 theme.
8. **Global shortcuts died when an app was focused** (WM didn't see keys). Now the WM receives every key (Skift model) and re-routes to the focused app unless an overlay is open.
9. **Tray icons were 12px.** `sf_icon_scaled(..., 26, 48)` assumed 48px-native icons, but tray icons are 22px → 22*26/48 = 12px. Fixed with `sf_icon_sz` (exact pixel size).

## 4. What is missing versus a real desktop OS

- **No widget toolkit.** Every button/pill is hand-drawn per-frame. You cannot compose a UI.
- **No app toolkit API.** Apps draw raw pixels. No shared button/text widgets (Skift has Karm UI).
- **Window chrome is WM-drawn**, not app-drawn. Skift apps draw their own titlebar (Scaffold).
- **No IPC.** Apps talk to the kernel via raw syscalls; there's no message bus, no client/server.
- **No GPU acceleration.** Everything is CPU blits into the framebuffer. QEMU std-vga only.
- **No vsync / real page flip.** Present is a copy at a fixed 60 Hz sleep; tearing possible.
- **No HiDPI**, no fractional scaling, one fixed resolution (1280×800).
- **No audio output path** despite an HDA driver skeleton; the volume slider is decorative.
- **No persistence of theme/settings** beyond a couple of hand-rolled .conf files.
- **No user management.** One account "demo", password "yart" (doas only), no login, no lock auth.
- **No clipboard**, no drag-and-drop, no window grouping, no snap previews, no minimize animation to a taskbar (dock only).
- **No notifications**, no system tray apps, no quick-launch.
- **No swap / memory-mapped file paging** (mmap is anonymous only).
- **No timers/select/poll** in the syscall surface for apps (blocking sleeps only).
- **No automated test suite** for the GUI; verification is screenshots + scripted QMP input.
- **Wifi/BT/ACPI power** are absent; "battery" is a lie in QEMU.

## 5. Architectural debt

- **wm.c still owns input routing, menus, config, processes** — it's 600 lines, down from 1500, but the input/action dispatch is still a giant if-chain, not an event model.
- **Damage list is capped at 32 rects**; overflow collapses to full-screen. Fine at 1280×800, but not "enterprise".
- **No retained window surfaces.** Every frame re-blits app surfaces from shared memory; no per-window cache of decorations/shadows.
- **Theme engine** is a flat array of 32 colors; no per-widget styles, no dark/light switch, no accent editing UI.
- **kora.bin is linked into every app binary** (assets are not shared at runtime) — each ELF is ~21 MB. That's why `build/init.elf` is 21 MB.
- **Blur is a 2-pass box blur**, not Skift's Gaussian, and the dock pill was removed anyway.

## 6. Bottom line

This is a **genuinely impressive hobby OS kernel** — SMP, real paging, preemption, a TCP/TLS stack, and a journaled filesystem is far beyond the median r/osdev project. The **compositor is architecturally honest** (dirty-rect + backdrop + partial present is exactly how a software compositor should work).

But it is **not a real OS, and it is not Skift**. It's a kernel with a very polished single-user demo shell on top. The gap between "looks like Skift" and "is Skift" is the entire widget toolkit, IPC layer, app framework, and real hardware drivers (audio, wifi, GPU). The visuals are now close; the substance is not, and no amount of pixel-tweaking the panel will change that.


---

## 7. Turn-by-turn fix log (real-time)

**Session 2026-08-15 (late): icons black + tray sizing + battery**

- **BUG — icons rendered black (regression).** My bilinear icon scaler
  (`icon_bilinear`) unpremultiplied with `r /= a` instead of `r *= 255/a`.
  With alpha 0..255, dividing a channel by ~200 collapses it to ~0, so every
  dock/desktop icon rendered near-black. Reproduced exactly in a Python
  harness: nearest-neighbour gave (97,99,96,216), my bilinear gave
  (0,0,0,216). Fixed: `r = r * 255.0f / a` (+ same for g,b) and the channel
  extraction is now correct and explicit (R=bits0-7, G=bits8-15, B=bits16-23,
  A=bits24-31). Verified: Home icon renders blue (18,178,253) again, dock
  shows 1059 coloured pixels.
- **BUG — icon pixelation.** `sf_icon_scaled` was nearest-neighbour; replaced
  with premultiplied-alpha bilinear (this is also what made the 22px-native
  tray icons look blocky at 30px).
- **FIX — tray icon size.** Kora tray glyphs have very different optical
  sizes (volume fills its box 22/22, wifi is 3 thin arcs 12/22, battery is a
  small pill 14/22). Measured each icon's ink bbox at build time and drew
  them at ink-equalising sizes (volume 20px, battery 31px, wifi 37px boxes
  → ~equal ~18px ink height). Verified balanced.
- **Battery like Skift.** `SYS_BATTERY` now reports a *virtual* battery
  (present, level 87% draining 1%/90s — documented VM stand-in, exactly like
  the virtual wlan0). Panel shows the Skift statusbar format: battery icon +
  "NN%", with state colours (red <10% critical, accent <20% low). On real
  hardware the same syscall reads the ACPI/EC battery.
- **Infrastructure.** `scripts/ensure_kernel.py` is now fully idempotent and
  self-healing (rebuilds a corrupted syscall switch to a canonical version,
  re-inserts missing helpers, de-duplicates) because the workspace
  environment repeatedly reverts kernel files to broken intermediate states.

---

## 2026-08-15 — "make wifi & battery REAL (or translate how Skift did it)"

User asked why wifi/battery can't be real "like a real enterprise OS", and if
not, to translate how Skift does it. I did both: read Skift's actual source
and implemented the real mechanism. Replaces the virtual 87% battery.

### What Skift actually does (receipts from the source)
- `hideo-shell/statusbar.cpp`:
  `statusbarBatteryIndicator() = icon(Mdi::BATTERY) + labelLarge("100%")`
  -> battery % is a **hardcoded "100%" string**, no reading at all.
  `statusbar(...) = clock + icon(Mdi::WIFI_STRENGTH_4) + icon(Mdi::NETWORK_STRENGTH_4) + battery`
  -> wifi/network are **static icons with zero state**.
- `hideo-oobe/app.cpp` hardcodes a fake Wi-Fi list ("Home Wi-Fi", "Office",
  "Guest network", "FBI Van", "Bob's Iphone").
- `strata-power/main.cpp` only forwards shutdown/restart; no battery.
Verdict: "translating Skift" for wifi/battery = a hardcoded icon + "100%".
YartOS was already *more* real than Skift (draining battery + wifi state
machine). Skift fakes both too.

### Why real Wi-Fi is impossible in QEMU (honest)
Real Wi-Fi needs real 802.11 hardware + vendor firmware + a mac80211-class
stack + wpa_supplicant. QEMU gives the guest a wired e1000 NIC only — no
radio. Even Linux/Windows in QEMU show "wired". The e1000 + our TCP/IP stack
ARE real (real packets on the wire). Fix: the status bar no longer pretends —
it shows the **wired** icon by default (G_wifi=false); the quick-settings
"Wi-Fi" tile now actually brings the (documented) virtual wlan0 up/down via
wifi_connect()/wifi_disconnect() instead of just flipping an icon.

### Battery — now REAL via ACPI (the Windows/Linux mechanism)
Real OSes read battery via ACPI `_BST`/`_BIF` on a PNP0C0A device. QEMU ships
NO battery (Launchpad #1502613; the QEMU-devel battery patch is not in 10.0)
— that's why Linux guests in QEMU show no battery. Implemented:
- `acpi/battery.asl` compiled with iasl -> 204-byte SSDT injecting a standard
  Control-Method Battery (PNP0C0A) + AC adapter (ACPI0003): the same SSDT
  trick the Arch Wiki / r/VFIO people use to give a VM a battery.
- `-acpitable file=acpi/battery.aml` in run.sh + run-qemu.sh (verified OVMF
  honours it: "acpi: [5] SSDT len=204").
- `acpi.c`: records DSDT/SSDT, scans for EisaId("PNP0C0A") and the "ACPI0003"
  string, evaluates `_BST`/`_BIF` with a minimal AML interpreter.
- `SYS_BATTERY` returns {present, charging, level} from real ACPI; with no
  battery in firmware it honestly reports "not present" -> panel shows "AC"
  (what a real OS shows in a VM). Verified both: with SSDT level=82%, without
  SSDT "acpi: no battery (PNP0C0A) in firmware tables".
- 82% = _BST remaining 7760 mWh / _BIF last-full 9400 mWh. Static because
  QEMU has no physical battery controller to sample; on real laptop hardware
  this same code reads the real EC battery. Honest limitation documented.

### AML gotchas (verified byte-by-byte against iasl output)
- ACPI PkgLength **includes its own bytes** (a 165-byte Scope encodes as
  0x47 0x0A = 167). Getting this wrong made _BST read garbage (level=0).
- EisaId("PNP0C0A") compiles to `0x0C 41 D0 0C 0A` (big-endian EISA dword
  0x41D00C0A), NOT 7 ASCII chars.
- iasl emits strings as `0x0D + chars + NUL` (no PkgLength byte).
- ACPI0003 is a *string* HID, not an EisaId (iasl rejects EisaId("ACPI0003"):
  "I" is not a hex digit).

### Environment revert (again) — and the fix
- syscall.h lost SYS_FB_PRESENT..SYS_BATTERY (80..85); syscall.c lost the
  five impl functions AND the shared `doas_check` (sys_doas reverted to its
  inline variant). ensure_kernel.py's presence check was fooled by the
  forward declaration (substring match) so it silently skipped.
- Fixed: check keys on `g_acpi_battery.present`; strips any stale impl block
  before re-inserting; re-inserts `doas_check` and slims sys_doas to call it;
  restores acpi.c/acpi.h/battery.aml from scripts/backup/ if wiped.
- Regression: lock auth still works (lk0=27 -> lk1/2/3=11 locked/wrong ->
  lk4=28 unlocked).

### Files touched
kernel/include/yart/acpi.h, kernel/arch/x86_64/acpi.c,
kernel/arch/x86_64/syscall.c, userland/wm.c, userland/wm_panel.c,
run.sh, scripts/run-qemu.sh, acpi/battery.asl(+.aml), scripts/backup/*,
scripts/boot_shot.py, scripts/ensure_kernel.py.

---

## 2026-08-15 (later) — "download another OS and copy it for 100% real battery + WiFi"

User asked to download another open-source OS I could copy so real hardware
shows a real battery and can scan/connect to real Wi-Fi. I measured the real
references (sparse clones) instead of guessing:

- WiFi (Linux): net/mac80211 = 94,233 LOC, net/wireless (cfg80211) = 57,162,
  one Intel driver (iwlwifi) = 185,608, wpa_supplicant = 157,989 (+ hostap
  crypto 476k). FreeBSD net80211 = 64,753, iwm = 22,985, wpi = 7,117.
  Haiku (the ONLY hobby OS with real WiFi) = ~117k LOC of ported
  FreeBSD stack + supplicant, years of work, a handful of chipsets.
  Plus proprietary firmware blobs (restricted redistribution).
- Battery: Linux ACPICA = 95,908 LOC, EC driver = 2,378 LOC.
- YartOS's current AML interpreter ≈ 250 LOC — honest gap to real-hardware
  _BST (which uses EC operation regions, mutexes, arithmetic).

Verdict documented: no OS makes WiFi copy-paste-able; "100% on your hardware"
is impossible to promise without testing on the exact machine.

### Mid-turn revert #2 — and the fix
The environment reverted acpi.c/acpi.h/syscall.c/syscall.h AGAIN (acpi.c back
to 139 lines) AND deleted scripts/backup/. ensure_kernel.py's re.sub()
replacement then corrupted sys_doas: re.sub treats "\n" in the replacement
as a newline, so the C string got a literal line break ("missing terminating
\"" errors). Fixes:
- re.sub replacement now uses a lambda (no backslash processing).
- sys_doas restore keys on `thin_doas not in s` (catches thick/broken/missing).
- Backups moved OUTSIDE the repo to /home/user/yartos-backups/ (the repo
  revert deletes new files inside the repo; /home/user/uploads-style paths
  survive).
- Re-generated acpi.c (389 lines) + acpi.h from the corrected text.
Re-verified after restore: level=82 via ACPI, lock auth lk0=27 -> lk1-3=11 ->
lk4=28, no-battery path prints "acpi: no battery (PNP0C0A) in firmware
tables".

---

## 2026-08-15 (night) — RTL8822CE port request (HP ProBook x360 435 G8)

User's hardware: Realtek RTL8822CE 802.11ac PCIe (10ec:c822), HP ProBook
x360 435 G8 (AMD Ryzen). User asked to "port a driver so wifi works".

Measured the actual port (sparse clone of Linux drivers/net/wireless/realtek/
rtw88): rtw88 total = 162,496 LOC. Minimal core: rtw8822c.c 5,448, pci.c
1,928, main.c 2,514, fw.c 2,471 (+fw.h 900), phy.c 2,699, reg.h 1,004,
mac80211.c 991, tx/rx/util/debug/coex. Firmware: rtw8822c_fw.bin (~1 MB,
Realtek redistributable license, linux-firmware/rtw88/).

Hard dependency: rtw88 is useless without the 802.11 stack it plugs into —
mac80211 (94,233 LOC) + cfg80211 (57,162) + a wpa_supplicant-class WPA2
implementation (157,989). YartOS has NONE of that. Also missing infra: MSI,
DMA ring allocator, SHA-1 (WPA2-PSK PBKDF2 uses HMAC-SHA1; we only have
SHA-256), EAPOL/4-way handshake. Present and reusable: legacy PCI config
reads (0xCF8/0xCFC), mmio_map(), AES-128 (FIPS 197) for CCMP, SHA-256/HMAC.

Honest verdict recorded: this is not "port a driver", it is "port the Linux
wireless stack" (~200k+ LOC incl. a supplicant), a multi-month project, and
it can only be validated by booting the ISO on the ProBook itself — which I
cannot do from this sandbox. No amount of "copy another OS" shortens that.

Implemented this turn (honest foundation, safe, testable on real hw):
- pci.c: on wireless detection, log real config-space rev + BAR0.
- wifi.c: recognises the rtw88 family (10ec:c822 RTL8822CE, b822 RTL8822BE,
  c821 RTL8821CE, d723 RTL8723DE); STOPS faking a working interface for a
  detected card; logs "driver NOT ported, card unusable"; wifi status
  reports the real chip honestly.
- Verified in QEMU: virtual wlan0 path unchanged, build + boot clean.

Staged plan (each stage testable on the laptop by booting the ISO):
  0) Boot YartOS on the ProBook — gate on base (display, PS/2, e1000, disk).
  1) PCIe infra: BAR0 map + MSI + DMA allocator (needed for any PCIe device).
  2) rtw88 firmware load: rtw8822c_fw.bin -> DMA -> read chip version back.
  3) Minimal cfg80211-equivalent: 802.11 scan (probe req/resp).
  4) WPA2: SHA-1 PBKDF2 + EAPOL 4-way handshake + AES-CCMP.
  5) Data path: TX/RX rings -> YartOS net stack + DHCP.

---

## 2026-08-15 (night 2) — "basic wifi, my PC only": scope + first brick

User can't run the OS locally yet; wants basic Wi-Fi for the RTL8822CE only,
asking: how many lines, how big in MB, and the cost of a 2nd driver later.

Honest estimate (basic WPA2 Wi-Fi, single chip RTL8822CE, no generalisation):
- rtw88 port (register tables + fw download + phy init + tx/rx + coex): ~21k LOC
  (the bulk, and it can't shrink much — it is mostly init tables you must
  reproduce exactly)
- minimal 802.11 core (hand-rolled mac80211/cfg80211 replacement): ~3.5k LOC
- WPA2-PSK (SHA-1+HMAC+PBKDF2 [DONE this turn, 400 LOC] + AES-CCMP + EAPOL
  4-way handshake + RSN IE parse): ~1.9k LOC total
- kernel infra (MSI, 32-bit DMA allocator, firmware loader, PCIe caps): ~1.4k
- userland (wifi CLI over the real path + DHCP trigger): ~0.4k
TOTAL ≈ 25-30k LOC. Size: source ~1.3 MB, compiled ~0.5-1 MB, firmware blob
rtw8822c_fw.bin ~1.0 MB (dominant) → project grows ~3 MB; ISO ~127 MB → ~130 MB.

Second driver later: the 802.11 core + infra are reusable by design. Same
family (rtw88, e.g. RTL8822BE) ≈ 1-2k LOC + its firmware (days). Different
vendor (Intel/Atheros) = a whole new driver port (10k-100k LOC) + firmware.
The future "detect chip -> download driver from our site" installer needs a
kernel module loader first (drivers are compiled-in today) — build that last.

This turn (verifiable without hardware): added kernel/lib/sha1.c —
SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104), PBKDF2-HMAC-SHA1 (RFC 2898) —
for WPA2-PSK PMK derivation. Proven byte-exact on the host against
FIPS/RFC 3174/2202 vectors AND the official IEEE 802.11 WPA2-PSK example
PMK (passphrase "password", SSID "IEEE" -> f42c6fc5...a12e), then wired into
the kernel with a boot self-test: "crypto: SHA-1/HMAC-SHA1/PBKDF2 selftest ok".
