/*
 * user.ld — linker script for AuraLite OS user-space programs.
 *
 * Links the program at a fixed user-space virtual address (0x40000000 = 1 GiB).
 * The kernel's ELF loader maps PT_LOAD segments at their p_vaddr and jumps to
 * the ELF entry point.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

USER_BASE = 0x40000000;

SECTIONS
{
    . = USER_BASE;

    .text ALIGN(16) : {
        *(.text .text.*)
    }

    .rodata ALIGN(16) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(16) : {
        *(.data .data.*)
    }

    .bss ALIGN(16) : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
