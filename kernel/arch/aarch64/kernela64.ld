/*
 * kernel/arch/aarch64/kernela64.ld -- linker script for the aarch64
 * kernel (ARM64_PLAN A0).
 *
 * Link at 0x40200000: 2 MiB above the virt board's RAM base
 * (0x40000000, plan Fact 3) -- clear of the DTB that QEMU parks AT
 * the RAM base for ELF payloads (Fact 2.2, the plan's day-one trap).
 * The riscv64 kernel sits 2 MiB above ITS RAM base for the OpenSBI
 * shadow; here the tenant below us is the DTB, but the arithmetic is
 * the same and so is the constant's justification.
 *
 * QEMU honours e_entry for ELF payloads on this machine -- unlike
 * OpenSBI, which cost V0 a debugging session by jumping to the
 * payload base.  .text.boot is placed first ANYWAY: "first byte of
 * the first segment" == "_start" is a fact all four kernels now hold
 * by construction, because it is free and because the V0 lesson says
 * entry-point indirection is exactly where silent garbage execution
 * is born.
 *
 * A0 is identity-linked (VMA == LMA), the stub-then-grow shape V0
 * used: A3 moves the kernel to the higher half (TTBR1, the D3
 * geometry) the way V3 did.  PHDRS split R+X / R / R+W from day one
 * so A3's W^X mapping reads section bounds, not guesses -- and .boot
 * is honestly RWX in its ELF header, the pre-paging world.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

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
    } :boot

    . = ALIGN(4096);

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(4096);

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    . = ALIGN(4096);

    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data

    . = ALIGN(16);

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
