/* Yart user binary - position-independent executable (PIE).
 * Loaded at a random base by the kernel's ASLR-aware loader, so the entry
 * (and all segments) are offsets from address 0. */
ENTRY(_start)

PHDRS
{
  headers PT_PHDR PHDRS;
  text    PT_LOAD FILEHDR PHDRS;
  data    PT_LOAD;
}

SECTIONS
{
    . = 0x1000;    /* leave room for the ELF + program headers */

    .text : { *(.text .text.*) } :text
    /* W^X: page-align every segment so NO page ever straddles the
     * text/data boundary.  The kernel loader maps each page with the
     * union of segment permissions; without this alignment the boundary
     * page would end up RWX.  With it, every user page is exactly RX or
     * RW, never both. */
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :data
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    /* .data.rel.ro holds pointer tables (e.g. char *argv[] = {...}) that
     * need R_X86_64_RELATIVE relocations.  Keep .rela.dyn in the FILE
     * (non-alloc, never mapped) so the kernel's ELF loader can apply the
     * load bias - discarding it left such pointers unrelocated (0xf018
     * instead of bias+0xf018) and exec'd binaries crashed. */
    .rela.dyn : { *(.rela.dyn) }
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) *(.rela.eh_frame*) *(.rela.plt*) *(.rela.got*) }
}
