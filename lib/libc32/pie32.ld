/*
 * lib/libc32/pie32.ld -- i386 static-PIE layout (RESIDUE2 T8, RES-18).
 *
 * Image base 0: a PIE's p_vaddrs/e_entry are offsets the loader seats
 * at ELF32_PIE_BASE (elf32load.c) and fixes up through .rel.dyn.
 * Segment permissions mirror user32.ld's treaty.
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS {
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0;
    .text : { *(.text .text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.note .note.*) *(.comment) }
}
