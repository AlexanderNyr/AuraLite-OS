/*
 * kernel/arch/riscv64/kernelrv.ld -- linker script for the rv64 kernel
 * (RISCV_PLAN V0).
 *
 * V0 links at physical 0x80200000: RAM on the QEMU virt machine starts
 * at 0x80000000 and OpenSBI owns the first 2 MiB, so 0x80200000 is the
 * conventional S-mode payload address (what -kernel loads, what
 * fw_payload builds expect on real boards).  VMA = LMA, paging off --
 * the same stub-then-grow shape kernel32.ld had at I1: V3 moves the
 * kernel to the Sv39 higher half and this script grows the .boot
 * physical-entry section then, not before.
 *
 * Same PHDRS discipline as the other two kernels' scripts:
 * differently-permissioned segments never share a page.  On THIS arch
 * the discipline is enforceable from V3 day one (Sv39 PTEs carry a
 * real X bit -- plan Fact 5), so the layout being right before paging
 * exists is the whole point.
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
    . = 0x80200000;

    .text : {
        /* .text.boot FIRST: OpenSBI jumps to the payload BASE
         * (0x80200000), not to e_entry -- measured in V0, see the
         * comment at _start in boot.S.  The linker script is what
         * makes "first byte" and "_start" the same fact. */
        KEEP(*(.text.boot))
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
        /* RISC-V small-data sections; __global_pointer$ would let us
         * use gp-relative addressing, but the kernel builds with
         * -mno-relax and does not set gp -- one less register with a
         * hidden contract in trap paths. */
        *(.sdata .sdata.*)
    } :data

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.riscv.attributes)
    }
}
