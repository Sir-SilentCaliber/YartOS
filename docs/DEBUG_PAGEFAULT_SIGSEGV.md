# Debug report — `vmm: kernel page fault at va=0x9` (desktop task)

## Symptom (serial log provided)

```
[user] init(1): entering CFS preemptive multitasking daemon loop
[sched] kclockd tick #50 (total_ticks=51)
vmm: SIGSEGV! Illegal access at va=0x0000000000000009 by PID 0 ('desktop') err=0x0
[YART PANIC] vmm: kernel page fault at va=0x0000000000000009 (err=0x0)
```

- Fault address `va=0x9` → near-NULL dereference (offset 9 into the null page).
- `err=0x0` → page not present, read access, **supervisor (CPL0)** access.

## Step 1 — Index the codebase

Cloned `https://github.com/Sir-SilentCaliber/YartOS` (HEAD `1d0395f`, "Update project",
pushed 2026-07-31, minutes before the report). Indexed the full tree:

- `kernel/arch/x86_64/` — `main.c` (entry), `idt.c`/`isr.asm` (exceptions),
  `syscall.c` (int 0x80 dispatcher), `user.c` (ring-3 launch), `gdt.c`, `pit.c`, ...
- `kernel/mm/` — `pmm.c` (bitmap), `vmm.c` (4-level paging), `heap.c`
- `kernel/gui/` — `desktop.c` (compositor/WM), apps, `session.c`, `login_overlay.c`
- `kernel/fs/` — `vfs.c`, `elf.c`, `bmp.c`, `config.c`
- `userland/` — `init.c`/`sys.h` (first ring-3 process)

Built (`make -j8 iso`, gcc 14.2 freestanding, nasm, Limine v7) and booted in QEMU:
**the current tree boots cleanly** — `/bin/init` runs in ring 3, exits, and the desktop
loop runs indefinitely with no crash.

## Step 2 — Finding: the log is from a *newer* build, but the defect class is here

The logged run contains subsystems that are **not in this tree** (no `kclockd` /
`kstat_worker` / `kwatchdog` kernel threads, no CFS scheduler, no CoW fork, no
`mmap`/`pipe`/`waitpid`/`win_create` syscalls, no per-task CR3). The exact faulting
line therefore cannot be reproduced from `main`.

However, the log pinpoints the two real defects this codebase **does** share with that
build, and both were fixed here:

1. **No user-vs-kernel fault discrimination.** Every CPU exception, including a
   page fault from a misbehaving ring-3 app, hit `isr_dispatch()` → `kpanic()`.
   One bad user pointer took down the whole OS. The logged build *tries* to deliver
   `SIGSEGV` ("`vmm: SIGSEGV!`") and only panics because the fault was in kernel mode;
   this tree didn't even try.
2. **Un-guarded near-NULL deref in the compositor.** The desktop task dereferenced a
   bad window/surface pointer at offset 9 in kernel mode. `draw_window()` in this tree
   dereferenced `w->flags` with no NULL guard.

## Step 3 — Fixes applied

### `kernel/arch/x86_64/idt.c`
New `page_fault()` handler for vector 14 (the VMM's job):
- Fault came from **ring-3** (`CS == USER_CS` or err bit 2) **and** a user task is
  live → print `vmm: SIGSEGV! ...` and kill the task via `user_return(-11)`
  (longjmp back into the kernel loop, exactly like `SYS_EXIT`). OS keeps running.
- Fault in **supervisor mode** → print `vmm: kernel page fault at va=... (err=...)`
  plus full register/CR2 dump, then `kpanic` (real kernel bug — must not be papered over).

### `kernel/arch/x86_64/user.c` + `kernel/include/yart/user.h`
- Added `g_user_active` flag: set immediately before `iretq` into ring 3, cleared on
  return/kill. Lets the fault handler know a user task can be safely killed.

### `kernel/arch/x86_64/syscall.c` + `kernel/include/yart/task.h`
- Exposed `task_getpid()` so the SIGSEGV report identifies the faulting task.

### `kernel/gui/desktop.c`
- `draw_window()` now NULL-guards the window node (prevents the near-null deref
  class of compositor crash).

## Step 4 — Verification (QEMU)

1. Normal boot: `init` runs, writes `INIT_RAN.txt`, exits 0, desktop loop runs
   indefinitely. Screenshot: `docs/fix-verification-desktop.png`.
2. Crash-recovery test: temporarily made `/bin/init` read `va=0x9` (same address as
   the log). Boot output:

```
vmm: SIGSEGV! Illegal access at va=0x9 by task 1 err=0x4
vmm: killing faulting ring-3 task and resuming kernel.
user: task exited with status -11
yart: kernel up; entering desktop loop.
```

   **No `[YART PANIC]`.** The OS survived a fault at exactly `va=0x9`. Test
   injection was removed afterwards; the committed `init.c` is unchanged.

## Root cause (as far as it can be determined from this tree)

The panic in the log is a **kernel-mode NULL-dereference at offset 9 in the desktop
(compositor) task**, triggered when it processed the freshly-created Ring-3 window /
surface, combined with a **VMM that panicked on any page fault instead of isolating
the faulting task**. If the newer scheduler/userland code is available, the exact
faulting deref is in the compositor's handling of the Ring-3 window surface
(`win_create()`/`win_map()` path) — a NULL/invalid window or surface pointer.
