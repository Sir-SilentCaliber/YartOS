# YartOS → "runs most Linux apps" — the path

**Goal:** make YartOS able to run the *majority* of real Linux userspace
programs (busybox, bash, curl, vim, python, tmux/htop, and eventually source
ports like the Doom family) by porting **musl libc** and completing the
**Linux syscall layer**.

**Honest scope:** this is the single biggest project in the repo. It is
measured in weeks-to-months, not flags. There is no "make apps work" switch —
but there *is* a well-known, proven path, and YartOS is already further along
it than most hobby OSes (it has a partial Linux ABI, a working dynamic-linker
test harness in `tests/ld-yart.c`, threads via clone+futex, a TCP stack, and
an ELF loader that understands ET_EXEC + PT_INTERP + auxv).

**Current baseline (audited `kernel/arch/x86_64/syscall.c`):**
`linux_dispatch()` handles **50 of 335** x86_64 syscalls. See
`scripts/syscall_matrix.py` for the full audit.

---

## The dependency ladder (why order matters)

```
musl libc ────── needs ──▶ the ~180 syscalls real apps use
     │
     ▼
musl ld.so ───── needs ──▶ full relocations + TLS + transitive .so loading
     │
     ▼
pthreads ─────── needs ──▶ clone flags + futex + set_tid_address + signals
     │
     ▼
apps (shell, tools, games) need ──▶ poll/epoll + ioctl + PTYs + /dev + /proc
```

Each layer unlocks a class of software. You cannot skip a layer.

---

## Phase 0 — done this session ✅
- Syscall coverage audit + `scripts/syscall_matrix.py` (repeatable).
- **Wave-1 quick-win syscalls** implemented in `linux_dispatch()`:
  pipe/pipe2, dup/dup3, fork/vfork, wait4, kill/tkill/tgkill, sched_yield,
  nanosleep/clock_nanosleep, clock_getres, fcntl (dupfd/getfd/setfd/getfl/setfl),
  fsync/fdatasync, truncate/ftruncate, umask, getrlimit/prlimit64, getrusage,
   sysinfo, getppid/getpgrp/getpgid/setsid, gettid, set_tid_address,
   sched_get/setaffinity, sync, getrandom.
   → **50 → 85 syscalls** handled, all the "trivial-but-everywhere" ones.

## Phase 1 — static musl (next concrete milestone)
**Goal: `busybox sh` runs.**
1. Add a `musl` build target: compile musl for x86_64 against YartOS's Linux
   ABI. musl supports custom ports via `arch/x86_64/syscall_arch.h` — define
   one syscall table that matches `linux_dispatch()`.
2. Build busybox static against that musl; package as an `.ypkg`; install
   over the network with `apk add`.
3. Fix whatever the loader/ABI trips on (this will surface real bugs: exec
   with more than 1 arg, brk/mmap edge cases, getcwd, fcntl flags).
**Exit test:** `apk add busybox` → `busybox sh -c 'ls; echo hi'` works in Nyra.

## Phase 2 — dynamic linking (musl ld.so)
**Goal: any dynamically-linked musl program runs.**
1. Finish the ELF loader (`kernel/arch/x86_64/user.c`): it already maps
   PT_INTERP and applies R_X86_64_RELATIVE. Add:
   - `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`, `R_X86_64_64`, `R_X86_64_COPY`
   - a real `.dynamic` walker (DT_NEEDED transitive loading, DT_SYMTAB/STRTAB/
     HASH/GNU_HASH, DT_PLTGOT/DT_JMPREL)
   - symbol resolution (global + lazy PLT binding)
2. TLS: `R_X86_64_TPOFF64`, `R_X86_64_DTPMOD64/DTPOFF64`, and initial-exec +
   general-dynamic models via `arch_prctl(ARCH_SET_FS)` (the kernel already
   stores/applies the FS base — build on it).
**Exit test:** `hello.so` linked against `libc.so` prints via `printf`, and a
`.so` referencing a program global (copy relocation) works — extending the
existing `tests/dynhello`/`copydemo`/`tlsdemo` suite to real musl.

## Phase 3 — threads & signals (musl pthreads)
**Goal: a `pthread` program runs correctly.**
1. `clone` flags: CLONE_VM/FILES/FS/SIGHAND/THREAD/SYSVSEM/PARENT_SETTID/
   CHILD_CLEARTID/SETTLS — the kernel has a base (shared PML4 refcount) but
   needs exit-time `set_tid_address` clearing + TLS-per-thread.
2. `futex`: WAIT/WAKE done; add WAIT_BITSET, WAKE_BITSET, REQUEUE.
3. Signals for real: `rt_sigprocmask` (currently a no-op — make it real),
   `rt_sigreturn` (Linux 15), `sigaltstack`, `SA_RESTORER`, `tgkill`,
   `set_robust_list`.
**Exit test:** a 4-thread mutex/condvar demo, and `kill -9`/SIGINT behaves.

## Phase 4 — the "majority of apps" long tail
**Goal: shells, curl, editors, file tools.**
1. IO multiplexing: `poll`, `select`, `epoll_create1/ctl/wait` (+ eventfd as
   the cheap wakeup primitive).
2. `ioctl`: the terminal subset (TCGETS/TCSETS/TIOCGWINSZ/termios) — this
   gates vim/tmux/htop.
3. File-`at` family: `newfstatat`, `unlinkat`, `renameat`, `mkdirat`,
   `faccessat`, `symlinkat`, `readlinkat`, `fchmodat`.
4. `mmap` completion: MAP_FIXED + file-backed mappings + `msync`/`madvise`/
   `mremap`.
5. Sockets completion: `sendmsg/recvmsg`, `setsockopt/getsockopt`,
   `getpeername/getsockname`, `accept4`, `socketpair`.
6. `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/tty` + a minimal `/proc`
   (`/proc/self/fd`, `/proc/mounts`, `/proc/meminfo`, `/proc/cpuinfo`).
**Exit test:** `curl` (static musl) fetches a URL; `busybox vi` edits a file.

## Phase 5 — PTYs + terminal apps
**Goal: tmux/htop/mc run.**
1. `/dev/ptmx` + `pts` device, `grantpt/unlockpt/ptsname`, `TIOCSPTLCK`.
2. Process groups/foreground-terminal signalling so Ctrl+C routes to the
   right job (the audit already flagged Nyra's missing PTY model — fix it
   here).
**Exit test:** `tmux` opens two panes; `htop` renders.

## Phase 6 — hardening + breadth
- Robust futexes, signal-driven IO, OOM correctness under fork storms,
  stack-growth faults + altstack, `getrandom` from real entropy.
- Native YartOS apps and Linux apps coexist; `apk` installs Linux apps with
  the musl runtime pulled in as a base package.
**Exit test:** a "torture" script: fork+mmap+threads+sockets under memory
pressure with no crash.

---

## The hard parts (don't underestimate)
| Area | Why it's hard | Where in repo |
|---|---|---|
| Dynamic linker | relocation + TLS correctness, symbol scope rules | `kernel/arch/x86_64/user.c`, `tests/ld-yart.c` |
| pthreads | clone exit paths, futex edge cases, TLS per-thread | `kernel/sched/sched.c`, `kernel/arch/x86_64/syscall.c` |
| poll/epoll/ioctl | block-until-ready without busy loops; termios state | `kernel/net/`, new `kernel/fs/` |
| mmap file-backing | page-cache semantics, MAP_FIXED unmap/remap | `kernel/mm/vmm.c` |
| /proc + PTYs | device model + session/foreground semantics | new kernel code |

## Recommended order of operations (per session)
1. Run `python3 scripts/syscall_matrix.py` → see the gap.
2. Implement the next batch from the current phase.
3. `make` (fast) — do NOT boot QEMU every time; boot only at phase
   milestones to verify a real program end-to-end.
4. Commit.

## Progress log
- **Session A:** matrix audit (`scripts/syscall_matrix.py`) + Wave-1 syscalls
  implemented in `linux_dispatch()` — coverage went **50 → 85 / 335**.
- **Session B:** **Phase 1 milestone reached.** Built musl (static) + a static
  busybox; **it boots and runs on YartOS** — `ls`, `cat`, `echo`, `seq`,
  `uname`, `pwd`, `sh` all work. Coverage now **139 / 335**. Kernel bugs fixed
  along the way (AT_PHDR for ET_EXEC, the `mkdir`↔`fchdir` syscall-number bug,
  region-table exhaustion + coalescing, `getdents64` pos-not-advancing (the
  `ls` OOM loop), `MAP_FIXED` for musl's brk-donation). Full recipe in
  `docs/HOWTO-build-busybox.md`.
  - Next: Phase 2 (dynamic linking) and the syscall long tail (`poll`/`select`/
    `epoll`, `ioctl`, `*at`, `sendmsg/recvmsg`) to run curl/vim.
