/*
 * lib/libca64/shella64.ld -- the aarch64 shell's layout (ARM64_PLAN
 * A5b).
 *
 * shellrv.ld's treaty at the fourth ISA: the shell links at
 * 0x30000000 so the programs it spawns (children at user_a64.ld's
 * 0x08048000) fit in the same address space by address-range treaty.
 * On this arch both halves live in TTBR0's tree (the A4 two-trees
 * fact); the map-over-existing-leaf refusal is the treaty's
 * enforcement, same as everywhere.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
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
