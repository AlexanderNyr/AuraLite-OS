/*
 * user.ld — linker script for AuraLite OS user-space programs.
 *
 * Links each program at a fixed user-space virtual address (0x40000000 = 1 GiB).
 * PT_LOAD segments are intentionally split and page-aligned so the kernel ELF
 * loader can enforce strict final permissions: text is RX, rodata is R/NX, and
 * data/bss is RW/NX.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

USER_BASE = 0x40000000;

PHDRS
{
    text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = USER_BASE;

    .text ALIGN(4096) : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data ALIGN(4096) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(16) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
