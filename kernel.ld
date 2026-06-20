/*
 * kernel.ld — NovOS kernel linker script for x86_64 higher-half (Limine-loaded).
 *
 * The kernel is linked at virtual address 0xFFFFFFFF80100000 (higher half).
 * Limine reads the PT_LOAD program headers, allocates physical pages, copies
 * the file image, and maps [p_vaddr, p_vaddr + p_memsz) to that physical memory.
 *
 * CRITICAL layout rule enforced by Limine: two PT_LOAD segments whose
 * permissions differ MUST NOT occupy the same page. We therefore page-align
 * (CONSTANT(MAXPAGESIZE) = 4096) every boundary between differently-permissioned
 * segments, and keep each PHDR's sections contiguous.
 *
 * p_flags: PF_X=1 (execute), PF_W=2 (write), PF_R=4 (read).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* R + X */
    rodata  PT_LOAD    FLAGS(1 << 2);              /* R     */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* R + W */
}

SECTIONS
{
    . = KERNEL_VMA + 0x100000;                 /* 0xFFFFFFFF80100000 */

    /* ---- Code (executable) ---- */
    .text : {
        *(.text .text.*)
    } :text

    . += CONSTANT(MAXPAGESIZE);

    /* ---- Read-only data ---- */
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . += CONSTANT(MAXPAGESIZE);

    /* ---- Writable data.
       Limine protocol requests live here: they must be in a loaded, WRITABLE
       segment (Limine writes the response pointer back into each request),
       and KEEP() prevents the linker from stripping these "unused" objects.
       Order matters: start marker -> requests -> end marker. ---- */
    .data : {
        *(.data .data.*)

        . = ALIGN(16);
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :data

    /* ---- Zero-initialised data (incl. the boot stack) ---- */
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
        *(.gnu.hash)
        *(.hash)
    }
}
