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
    .rodata : { *(.rodata .rodata.*) } :data
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) *(.rela.*) }
}
