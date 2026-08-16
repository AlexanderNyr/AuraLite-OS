/*
 * lib/libc32/shell32.ld -- the i386 shell's layout (I386_PLAN I7).
 *
 * Same script as user32.ld, one number different: the shell links at
 * 0x30000000 so the programs it spawns (children at user32.ld's
 * 0x08048000) fit in the same address space without window collisions
 * -- there is ONE shared page directory at I7, not per-process
 * directories, so parent and child coexist by address-range treaty.
 * paging32_map's already-mapped refusal is the treaty's enforcement.
 */

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* R + X */
    rodata  PT_LOAD    FLAGS(1 << 2);              /* R     */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* R + W */
}

SECTIONS
{
    . = 0x30000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.note .note.*) *(.comment)
    }
}
