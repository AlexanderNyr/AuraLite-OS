/*
 * kernel/arch/i386/kernel32.ld -- linker script for the i386 kernel
 * (I386_PLAN I2, higher-half since I3).
 *
 * I3 layout: the kernel LIVES at virtual KERNEL_VMA + phys (0xC0100000
 * region, plan D3) but is LOADED at physical 0x00100000 -- Stage 2's
 * 32-bit path still enables no paging, so a small .boot section stays
 * VMA = LMA at 0x00100000, builds a minimal PSE page directory, turns
 * on CR0.PG and jumps to the higher half.  Every other section carries
 * an AT() clause deriving its LMA by subtracting KERNEL_VMA, which
 * keeps the load image contiguous from 0x00100000 -- the property
 * elf32.inc's forward copy from the 0x00200000 staging buffer depends
 * on (dst = src - 0x100000 stays below src for every byte).
 *
 * ENTRY is the physical .boot entry: e_entry is what Stage 2 jumps to,
 * and Stage 2 runs paging-off.
 *
 * Same PHDRS discipline as kernel.ld: differently-permissioned
 * segments never share a page (minus NX, which non-PAE i386 does not
 * have -- I386_PLAN D3).
 */

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

KERNEL_VMA = 0xC0000000;

PHDRS
{
    boot    PT_LOAD    FLAGS((1 << 0) | (1 << 1) | (1 << 2)); /* RWX: code + boot page dir */
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2));            /* R + X */
    rodata  PT_LOAD    FLAGS(1 << 2);                         /* R     */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2));            /* R + W */
}

SECTIONS
{
    /* ---- Physical-mode entry: VMA = LMA = 0x00100000 ---- */
    . = 0x00100000;

    .boot : {
        *(.boot)
    } :boot

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* ---- The higher-half kernel proper ---- */
    . += KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VMA) {
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
