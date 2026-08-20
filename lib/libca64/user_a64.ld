/*
 * lib/libca64/user_a64.ld -- aarch64 user program layout (ARM64_PLAN
 * A5b).
 *
 * user_rv.ld's numbers at the fourth ISA: link at the classic
 * 0x08048000, inside the loader's shared ELF window
 * [0x08000000, 0x40000000) -- the constant transfers unchanged (plan
 * A5b task text).  Headers not loaded (the I5 bare--Ttext lesson).
 *
 * PHDRS discipline as everywhere; on aarch64 the loader will turn
 * p_flags into UXN/PXN-bearing PTEs (A5c), so the layout is enforced
 * with both execute-never bits, not aspirational.
 *
 * [AMEND-7]: --gc-sections rides on the LINK LINE (Makefile), not in
 * this script -- O8's x86 measurement (initrd -65%) transfers, and
 * one measured fact travels with it: lld roots SHT_INIT_ARRAY
 * sections by built-in rule, so if/when init_array tables arrive here
 * (the full libc port, A8), KEEP is belt-and-braces, not load-bearing.
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
