/* Yart user binary - link to a flat virtual address. */
ENTRY(_start)
SECTIONS
{
    . = 0x0000000040000000;

    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
