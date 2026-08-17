// ============================================================================
// kernel/arch/aarch64/boot.S -- aarch64 kernel entry point (ARM64_PLAN A0).
//
// QEMU's virt machine loads the ELF and enters _start at EL1 directly
// (measured in fact-finding, plan Fact 2.1): no BIOS, no UEFI, no
// OpenSBI-equivalent stage runs before this instruction.  PSCI exists
// as a *service* reachable over hvc (D2), not as a boot stage.
//
// Register state at entry, ELF-payload edition -- and this is the
// plan's day-one trap, so it is spelled out:
//   x0 = 0.  NOT the DTB pointer.  The Linux `Image` boot protocol's
//        "x0 = DTB phys" promise applies to raw images only; for ELF
//        payloads QEMU parks the DTB at the BASE OF RAM (0x40000000),
//        FDT magic verified there during fact-finding (Fact 2.2).
//        x0 is preserved and *reported* by kmain_a64 so the measured
//        fact stays regression-covered, but nothing dereferences it.
//   CurrentEL = EL1 (EL2 only under -machine virt,virtualization=on,
//        which D1 refuses -- see the check below).
//
// .text.boot is placed FIRST by kernela64.ld.  Unlike OpenSBI, QEMU
// honours e_entry for ELF payloads here -- but the V0 lesson
// ("first byte of the first segment" and "_start" must be the same
// fact by construction) cost a whole debugging session on riscv64 and
// costs nothing to keep, so all four kernels share the discipline.
// ============================================================================

.section .text.boot
.globl _start
_start:
    // ---- 0. Preserve x0: kmain_a64 reports it (the measured-fact
    // echo), nothing trusts it. --------------------------------------
    mov   x19, x0

    // ---- 1. D1: this kernel is an EL1 citizen.  CurrentEL != EL1
    // gets an honest refusal banner, not a limp-along -- the I0
    // lesson, fourth edition.  (Measured: virtualization=on enters at
    // EL2; the refusal path is regression-covered by the smoke.) -----
    mrs   x1, CurrentEL
    lsr   x1, x1, #2
    cmp   x1, #1
    b.eq  .Lel_ok

    adr   x2, refusal_msg
    bl    boot_puts
    // No power-off here: PSCI over hvc from EL2 would trap into EL2's
    // own vector (us, with no vector table) -- park instead.  The
    // smoke asserts the banner, not an exit code, on this path.
.Lpark:
    wfi
    b     .Lpark

.Lel_ok:
    // ---- 2. Mask D,A,I,F: no interrupt may arrive before A2 installs
    // a vector table.  (QEMU enters with them masked; assert-shaped
    // code masks anyway rather than assuming.) ------------------------
    msr   daifset, #0xf

    // ---- 3. Stack up, .bss cleared.  The stack lives in .bss and
    // nothing is on it yet -- the same ordering note as the riscv64
    // entry: set sp first, zero everything, re-set nothing. -----------
    ldr   x1, =boot_stack_top
    mov   sp, x1

    ldr   x1, =__bss_start
    ldr   x2, =__bss_end
.Lbss_clear:
    cmp   x1, x2
    b.hs  .Lbss_done
    str   xzr, [x1], #8
    b     .Lbss_clear
.Lbss_done:

    // ---- 4. kmain_a64(x0_at_entry). ---------------------------------
    mov   x0, x19
    bl    kmain_a64

    // ---- 5. Not reached in a healthy boot (kmain powers off via
    // PSCI). ----------------------------------------------------------
.Lpark_high:
    wfi
    b     .Lpark_high

// boot_puts(x2 = NUL-terminated string): PL011 TX with TXFF polling.
// Assembly because the refusal path must not depend on any C having a
// valid stack or .bss; clobbers x2-x5 only.
boot_puts:
    ldr   x3, =0x09000000         // PL011 base (virt board, plan Fact 3)
1:  ldrb  w4, [x2], #1
    cbz   w4, 3f
2:  ldr   w5, [x3, #0x18]         // FR
    tbnz  w5, #5, 2b              // TXFF set -> spin
    str   w4, [x3]                // DR
    b     1b
3:  ret

.section .rodata
refusal_msg:
    .asciz "AuraLite aarch64: CurrentEL != EL1; refusing to boot (ARM64_PLAN D1).\n"

// Physical layout, exported as data: contract 2 of the shared DTB
// walker (kernel/dt/fdt.h) -- fdt_parse marks [kernel_layout[0],
// kernel_layout[7]) as BOOT_MEM_KERNEL so the allocator never hands
// out the image's own pages.  The pool shape (not bare externs) is
// riscv64's: medany C there cannot address absolute low symbols
// across the HHDM gap, so the CONTRACT is a data pool on every arch
// -- one walker, one access pattern, no per-arch #ifdef in shared
// code.  Slots [1..6] serve A3's paging the way V3 used them.
.align 3
.globl kernel_layout
kernel_layout:
    .quad __kernel_start          // [0] image start, PHYS
    .quad __kernel_start          // [1] (A3 fills the boot/text split)
    .quad __text_start            // [2] .text start
    .quad __text_end              // [3]
    .quad __rodata_start          // [4]
    .quad __rodata_end            // [5]
    .quad __data_start            // [6] .data start
    .quad __kernel_end            // [7] image end, PHYS

.section .bss
.align 16
boot_stack_bottom:
    .space 64 * 1024
.globl boot_stack_top
boot_stack_top:
