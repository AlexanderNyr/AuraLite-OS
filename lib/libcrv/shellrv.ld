/*
 * lib/libcrv/shellrv.ld -- the rv64 shell's layout (RISCV_PLAN V5).
 *
 * shell32.ld's treaty at the third width: the shell links at
 * 0x30000000 so the programs it spawns (children at user_rv.ld's
 * 0x08048000) fit in the same address space -- one shared root table
 * at V5, not per-process tables, so parent and child coexist by
 * address-range treaty.  paging_rv_map over an existing leaf refuses,
 * which is the treaty's enforcement.
 */

OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)
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
        *(.srodata .srodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.note .note.*) *(.comment)
        *(.riscv.attributes)
    }
}
