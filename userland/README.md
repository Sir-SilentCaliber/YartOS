# Userland

Ring-3 binaries for Yart OS.  Everything in here is compiled freestanding
(no glibc) against the syscall ABI in `sys.h` and linked with
`userland/init.ld` (PIE, ASLR-friendly).

## Layout

| File          | What it is                                                        |
|---------------|-------------------------------------------------------------------|
| `start.c`     | crt0: reads argc/argv/envp off the stack (the kernel's exec path  |
|               | places a SysV process image there) and calls `main_entry`.        |
| `sys.h`       | syscall numbers + inline wrappers (fast `syscall` instruction)    |
|               | + a tiny freestanding libc subset (write/read/open/...).          |
| `init.c`      | `/bin/init`: the boot-time process.  Runs the boot test suite     |
|               | (persistence, FPU, NX, permissions/doas, fault isolation, kill,   |
|               | mmap/brk, fsync, ACL, signals, exec, SMP) then hands the screen   |
|               | to `wm_run()` - the ring-3 compositor.                            |
| `wm.c`        | The compositor: panel, dock, cursors, tween animations, clock.    |
|               | Talks to the kernel only through FB_INFO/FB_FLIP/POLL_KEY/        |
|               | POLL_MOUSE/TIME_MS.                                               |
| `gfx.c`       | Software renderer used by the compositor (blit, blend, rounded    |
|               | rects, fonts, Kora icons from the embedded asset blob).           |
| `hello.c`     | `/bin/hello`: the exec() demo - prints argv/envp, exits 7.        |

## The process image convention

When the kernel execs (or boots) a program it writes, on the new stack:

```
[argc:8] [argv[0..argc-1]:8*argc] [NULL] [envp[0..]:8*envc] [NULL] [strings]
```

`_start` (start.c) picks these up exactly like a real OS and calls
`int main_entry(int argc, char **argv, char **envp)`.

## Build

`make` builds `build/init.elf` (compositor) and `build/hello.elf`
(exec demo) and copies them into the initrd as `/bin/init` and
`/bin/hello`.  Syscalls are the fast `syscall` instruction; `int $0x80`
remains as a kernel-side fallback.
