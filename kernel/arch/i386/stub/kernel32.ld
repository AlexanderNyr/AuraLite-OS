/*
 * kernel/arch/i386/stub/kernel32.ld -- linker script for the i386 boot
 * stub (I386_PLAN I1).
 *
 * Unlike kernel.ld (higher-half, VMA 0xFFFFFFFF80100000, phys derived by
 * subtraction in elf.inc), the stub is linked with VMA = LMA = 0x00100000:
 * Stage 2's 32-bit path enables no paging, so p_paddr is the destination
 * verbatim and the entry jump is a physical address.  The real i386
 * kernel (I3) moves to 0xC0100000 higher-half once it owns paging; this
 * script is scoped to the stub and dies with it.
 *
 * Same PHDRS discipline as kernel.ld: differently-permissioned segments
 * never share a page, so the future paging code can map them W^X-honest
 * (minus NX, which non-PAE i386 does not have -- I386_PLAN D3).
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
    . = 0x00100000;

    .text : {
        *(.text .text.*)
    } :text

    . += CONSTANT(MAXPAGESIZE);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . += CONSTANT(MAXPAGESIZE);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
