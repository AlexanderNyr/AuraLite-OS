/*
 * lib/libc32/user32.ld -- i386 user program layout (I386_PLAN I5).
 *
 * The first cut used a bare -Ttext 0x08048000 and lld emitted an extra
 * headers-only PT_LOAD at its default 0x00400000 image base -- outside
 * [ELF32_USER_MIN, ELF32_USER_MAX), which elf32load_map correctly
 * refused.  The loader was right and the link was wrong, hence this
 * script: everything inside the window, headers not loaded, and the
 * same differently-permissioned-segments-never-share-a-page rule the
 * kernel scripts follow (even though non-PAE i386 cannot enforce the
 * distinction -- plan D3 -- the layout keeps the sweep honest for when
 * binaries are shared with a PAE-capable future).
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
    . = 0x08048000;

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
