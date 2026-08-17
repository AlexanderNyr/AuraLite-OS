/*
 * lib/libcrv/user_rv.ld -- rv64 user program layout (RISCV_PLAN V5).
 *
 * user32.ld's numbers at the third width: link at the classic
 * 0x08048000, inside the loader's [ELF_RV_USER_MIN, ELF_RV_USER_MAX)
 * window, headers not loaded (the I5 bare--Ttext lesson: lld's
 * default image base put a headers-only PT_LOAD outside the window
 * and the loader refused it -- the script IS the fix).
 *
 * PHDRS discipline as everywhere -- and on THIS arch the loader turns
 * p_flags into real PTE bits (PF_X without W, PF_W without X), so the
 * layout is enforced, not aspirational.
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
    . = 0x08048000;

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
