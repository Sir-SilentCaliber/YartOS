# Syscall ABI — YartOS vs Linux vs POSIX (brutal honesty)

Question: "does the OS use the same Linux/POSIX syscalls exactly, and what
possibilities does that give us?"

Answer: **No — not the same syscalls, but the same ABI and the same POSIX
names/semantics for the core set.** The exact situation:

## 1. What is the SAME as Linux x86_64

- **The ABI.** `syscall` instruction, `sysret` return, arguments in
  `rdi, rsi, rdx, r10`, return value in `rax` — byte-for-byte the System V
  AMD64 calling convention Linux uses. There is also an `int 0x80`
  fallback.
- **The binary format.** Position-independent ELF64 (`ET_DYN`), SysV
  startup image on the stack (`argc, argv[], envp[]`), NX + ASLR. A YartOS
  executable and a Linux PIE executable are structurally identical.
- **The names + semantics of the core set.** `fork, exec, waitpid, open,
  read, write, close, lseek, getdents, mkdir, unlink, stat, getcwd, chdir,
  getpid, chmod, kill, mmap, munmap, brk, sigaction, fsync, rename,
  setuid, setgid, umask, truncate, pipe, getcpu` — all POSIX-named, all
  POSIX-flavoured behaviour (CoW fork, demand-paged mmap, SIGKILL, PBKDF2
  password auth, real VFS with permission bits, fsync durability).

## 2. What is DIFFERENT

- **The syscall NUMBERS are a custom table.** The only number that matches
  Linux is `write == 1`. Everything else collides with something else:

  | YartOS # | name      | Linux # | Linux name |
  |---------:|-----------|--------:|------------|
  |  0 | EXIT     |  60 | exit |
  |  1 | WRITE    |   1 | write  ← only match |
  |  2 | READ     |   0 | read |
  |  3 | OPEN     |   2 | open |
  |  4 | CLOSE    |   3 | close |
  |  5 | LSEEK    |   8 | lseek |
  |  6 | GETDENTS |  78/217 | getdents(64) |
  |  9 | STAT     |   4 | (new)stat |
  | 12 | GETPID   |  39 | getpid |
  | 14 | YIELD    |  24 | sched_yield |
  | 17 | FORK     |  57 | fork |
  | 18 | WAITPID  |  61 | wait4 |
  | 22 | KILL     |  62 | kill |
  | 23 | MMAP     |   9 | mmap |
  | 27 | BRK      |  12 | brk |
  | 28 | SIGACTION|  13 | rt_sigaction |
  | 30 | FSYNC    |  74 | fsync |
  | 45 | EXEC     |  59 | execve |
  | 53 | PIPE     |  22 | pipe |

  So a stock Linux binary executing `syscall` with Linux numbers would hit
  **completely wrong handlers** and crash instantly.

- **Struct layouts differ.** `dirent`, `stat`, `sigaction` are custom
  (`yart_dirent_t`, `yart_stat_t`, …), not Linux's `linux_dirent64`,
  `struct stat`, `rt_sigframe`.

- **The I/O model differs.** Linux apps do everything through fd-based
  `socket/bind/listen/accept/connect + poll/select/epoll`. YartOS has its
  own flat `SYS_TCP_CONNECT/SEND/RECV/LISTEN/ACCEPT`, `SYS_UDP_SEND/RECV`,
  `SYS_DNS_RESOLVE`, `SYS_TLS_*` — no fd-based sockets, no `poll`, no
  `select`, no `epoll`, no `ioctl`.

- **No threads.** `fork` only; no `clone`, no `futex`, no `exit_group`,
  no `rt_sigprocmask`. pthreads cannot work yet.

- **YartOS-specific syscalls Linux doesn't have** (our "Wayland + systemd"
  in one): `FB_INFO/FLIP/PRESENT, POLL_KEY/POLL_MOUSE, WM_CREATE/FLIP/
  SCAN/FOCUS/DESTROY/TITLE/MOVE/RESIZE, NOTIFY(_POLL), BATTERY,
  CLIPBOARD_SET/GET, AUTH_VERIFY, PASSWD, DOAS, DROP, ACL, KLOG, DMESG,
  NET_INFO, WIFI_SCAN/CONNECT/STATUS/DISCONNECT, AUDIO_VOL, MOUSE_POS`.

## 3. What possibilities this gives us

1. **Source-level POSIX porting — already works today.** Because the names
   and semantics follow POSIX, any program written against POSIX can be
   recompiled for YartOS by providing the handful of syscalls it uses
   (this is how Files/Editor/Settings/nyra exist). The sys.h wrappers
   already look like a mini-libc.

2. **Binary Linux compatibility — the big lever.** If we add a *Linux
   compatibility layer* to the dispatch switch — map Linux x86_64 syscall
   numbers onto the same handlers and match the `stat`/`dirent`/`sigaction`
   struct layouts — then **statically-linked Linux binaries run unmodified**.
   That single change buys: BusyBox, toybox, curl, dropbear (SSH), bash,
   dash, Python/Node/Go if built static-musl. This is exactly what
   "portable hobby OS + real userland" projects do (SerenityOS,
   Managarm, ToaruOS all run a BusyBox/static-ELF world before their own
   libc is mature).

3. **Port musl once → a whole ecosystem.** musl is a clean, small libc
   (~10k LOC core). Porting it (it's mostly the syscall layer + ELF
   loader) gives POSIX threading, malloc, stdio, and makes thousands of
   real programs buildable natively.

4. **Linux test suites as a correctness oracle.** With the Linux ABI
   mapping, we can run parts of LTP / POSIX conformance tests against the
   kernel — real, reproducible proof of "enterprise OS" behaviour instead
   of our own selftests.

## 4. Honest blockers (what we must build first)

- Renumber the table or add a Linux-number mapping in `syscall_handler`.
- Match `struct stat`, `linux_dirent64`, `rt_sigaction` layouts.
- Implement `socket/bind/listen/accept/connect/send/recv` as fd-based
  wrappers over the existing TCP/UDP stack, plus `poll`/`select`.
- `clone`/`futex` (or a minimal `clone` for threads) — the hardest part.
- A dynamic loader (`ld-musl`) if we want shared-lib binaries (static
  first avoids this entirely).

**Recommendation:** phase 1 = static-musl Linux ABI compatibility layer
(numbers + structs + fd sockets) → run BusyBox unmodified; phase 2 = port
musl; phase 3 = threads. That is the single highest-leverage "possibility"
the current architecture gives us, because the ABI, ELF loader and VFS are
already Linux-shaped.
