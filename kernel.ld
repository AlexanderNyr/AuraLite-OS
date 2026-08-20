/*
 * kernel.ld -- AuraLite OS kernel linker script for x86_64 higher-half.
 *
 * The kernel is linked at virtual address 0xFFFFFFFF80100000 (higher half).
 * The bootloader (BIOS Stage 2, UEFI BOOTX64.EFI, or the optional Limine
 * fallback) reads the PT_LOAD program headers, allocates physical pages,
 * copies the file image, and maps [p_vaddr, p_vaddr + p_memsz) to that
 * physical memory.
 *
 * Layout rule (enforced by every loader we target): two PT_LOAD segments
 * whose permissions differ MUST NOT occupy the same page.  We therefore
 * page-align (CONSTANT(MAXPAGESIZE) = 4096) every boundary between
 * differently-permissioned segments and keep each PHDR's sections
 * contiguous.
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

/* OPT_O8 KEEP() audit (--gc-sections is on): the roots that keep the
 * kernel alive are ENTRY(_start) and the relocations OUT of the NASM
 * objects (isr_stubs.asm -> isr/irq dispatch, syscall_entry.asm ->
 * syscall_dispatch, context/boot asm -> their C counterparts); NASM
 * emits one .text per object, each referenced from C by name, so the
 * graph is closed in both directions.  There are NO sections in this
 * kernel reachable ONLY through linker-script symbols (the one class
 * gc-sections silently kills): no .init_array (kernel constructors are
 * explicit *_init() calls from kmain), no driver-registration tables,
 * no per-CPU section tricks -- __bss_start/__bss_end frame a section
 * the output always has.  So: zero KEEPs, and this comment is the
 * audit trail.  The load-bearing KEEP in this tree is user.ld's
 * .init_array one; the O8 negative control proves it. */
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

    /* ---- Writable data ---- */
    .data : {
        *(.data .data.*)
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
