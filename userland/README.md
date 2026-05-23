# Userland

Future home for ring-3 binaries.  The kernel already sets up a user code/data
descriptor (selectors `0x18` / `0x20`) and a TSS with a kernel stack on
`RSP0`.  A minimal `iretq` to ring-3 with an entry pointing to a flat blob of
user code mapped with `PTE_US` will give you ring-3 execution.

The pattern is:

```
push USER_DS        ; ss
push user_stack_top ; rsp
push user_rflags    ; rflags (IF set)
push USER_CS        ; cs
push user_entry     ; rip
iretq
```

Bootstrap a single user task by allocating two pages, mapping them at
`0x0000_0000_4000_0000` with `PTE_US|PTE_RW`, copying a tiny `int 0x80`
binary into the first page, and `iretq`-ing into it.  Add a `syscall_dispatch`
vector at IDT 0x80 and you have userland.
