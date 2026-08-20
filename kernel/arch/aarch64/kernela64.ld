/*
 * kernel/arch/aarch64/kernela64.ld -- linker script for the aarch64
 * kernel (ARM64_PLAN A0; higher-half since A3).
 *
 * A3 layout, kernelrv.ld's pattern at the same geometry:
 *
 *   .boot   VMA = LMA = 0x40200000 -- the physical-entry world.
 *           2 MiB above the RAM base, clear of the DTB QEMU parks AT
 *           the base (Fact 2.2).  _start runs here with the MMU off:
 *           the EL1 check, SPSel, the early TTBR0/TTBR1 tables (also
 *           in this section -- they must exist before any allocator),
 *           MAIR/TCR/SCTLR, and the literal-pool jump high.
 *
 *   everything else: VMA = HHDM + physical, LMA = physical (AT()).
 *           The D3 direct-map constant 0xFFFFFFC000000000 makes
 *           "kernel virtual address" and "HHDM view of its physical
 *           address" the SAME number -- and on THIS arch the constant
 *           falls out of TTBR1 arithmetic: T1SZ=25 gives a 512 GiB
 *           TTBR1 window at 0xFFFFFF8000000000, and the HHDM sits
 *           256 GiB in, at level-1 index 256 -- the same index the
 *           same constant hits in Sv39.  Equal by computation, not
 *           copy-paste (the claim check asserts both).
 *
 * PHDRS discipline: differently-permissioned segments never share a
 * page, and from A3 on the permissions are real twice over -- AArch64
 * PTEs carry AP read-only AND two execute-never bits (UXN/PXN), so
 * paging_a64.c maps .text RX, .rodata R, .data/.bss RW from the
 * section bounds exported here.  The .boot segment is honestly RWX in
 * its ELF header: it is the pre-paging world, and the final tables
 * leave it as plain HHDM RW frames.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

HHDM = 0xFFFFFFC000000000;

PHDRS
{
    boot    PT_LOAD    FLAGS((1 << 0) | (1 << 1) | (1 << 2)); /* R+W+X */
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2));            /* R + X */
    rodata  PT_LOAD    FLAGS(1 << 2);                         /* R     */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2));            /* R + W */
}

SECTIONS
{
    . = 0x40200000;

    __kernel_start = .;

    .boot : {
        KEEP(*(.text.boot))     /* _start == first byte: the shared discipline */
        *(.boot.data)           /* early TTBR0/TTBR1 roots */
    } :boot

    . = ALIGN(4096);
    __boot_end_phys = .;

    /* The higher half: VMA up high, LMA continues physically. */
    . = HHDM + __boot_end_phys;

    .text : AT(ADDR(.text) - HHDM) {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    } :text

    .rodata : AT(ADDR(.rodata) - HHDM) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    } :rodata

    .data : AT(ADDR(.data) - HHDM) {
        __data_start = .;
        *(.data .data.*)
    } :data

    . = ALIGN(16);

    .bss : AT(ADDR(.bss) - HHDM) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    __kernel_end_virt = .;
    __kernel_end = __kernel_end_virt - HHDM;    /* physical, for the pool */

    /* A5a: the Image header's image_size field -- the whole memory
     * footprint (bss included, per the arm64 boot protocol) measured
     * from the load base.  Linker arithmetic, not a hand-kept number. */
    __image_size = __kernel_end - 0x40200000;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
