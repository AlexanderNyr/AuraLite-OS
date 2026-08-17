/*
 * kernel/arch/riscv64/kernelrv.ld -- linker script for the rv64 kernel
 * (RISCV_PLAN V0; higher-half since V3).
 *
 * V3 layout, the kernel32.ld pattern one level deeper:
 *
 *   .boot   VMA = LMA = 0x80200000 -- the physical-entry world.
 *           OpenSBI jumps to the payload BASE (0x80200000, NOT
 *           e_entry -- measured in V0), so .text.boot stays the first
 *           byte of the first segment.  _start runs here with paging
 *           off: hart lottery, the early Sv39 root table (also in
 *           this section -- it must exist before any allocator), the
 *           satp write, and the absolute jump high.
 *
 *   everything else: VMA = HHDM + physical, LMA = physical (AT()).
 *           The D3 direct-map constant 0xFFFFFFC000000000 makes
 *           "kernel virtual address" and "HHDM view of its physical
 *           address" the SAME number -- the same trick kernel32.ld
 *           played at 0xC0000000, with 256 GiB of headroom instead
 *           of 896 MiB.
 *
 * PHDRS discipline: differently-permissioned segments never share a
 * page, and on THIS arch the permissions are real from V3 on -- Sv39
 * PTEs carry a genuine X bit, so paging_rv.c maps .text RX, .rodata
 * R, .data/.bss RW from the section bounds exported here.  The .boot
 * segment is honestly RWX in its ELF header: it is the pre-paging
 * world, and the final tables map it read-only.
 */

OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)
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
    . = 0x80200000;

    /* Image bounds, PHYSICAL, as absolute symbols.  C code cannot
     * take their address directly (medany's auipc cannot reach an
     * absolute 0x80200000 from the higher half); boot.S exports them
     * through the kernel_phys_bounds literal pool instead. */
    __kernel_start = .;

    .boot : {
        KEEP(*(.text.boot))     /* _start == first byte: the V0 fact */
        *(.boot.data)           /* early root table + hart lottery */
    } :boot

    . = ALIGN(4096);
    __boot_end_phys = .;

    /* ---- the higher-half switch: VMA += HHDM, LMA stays physical ---- */
    . = . + HHDM;

    __text_start = .;
    .text : AT(ADDR(.text) - HHDM) {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : AT(ADDR(.rodata) - HHDM) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data : AT(ADDR(.data) - HHDM) {
        *(.data .data.*)
        /* RISC-V small-data sections; __global_pointer$ would let us
         * use gp-relative addressing, but the kernel builds with
         * -mno-relax and does not set gp -- one less register with a
         * hidden contract in trap paths. */
        *(.sdata .sdata.*)
    } :data

    .bss : AT(ADDR(.bss) - HHDM) {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    __kernel_end = . - HHDM;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.riscv.attributes)
    }
}
