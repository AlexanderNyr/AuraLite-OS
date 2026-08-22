// ============================================================================
// kernel/arch/aarch64/boot.S -- aarch64 kernel entry point (ARM64_PLAN A0;
// higher-half since A3).
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
//   CurrentEL = EL1; SPSel = 0 (measured in A2 -- the kernel DECLARES
//        SPSel=1 below instead of inheriting the reset value).
//
// A3 shape (boot.S-of-riscv64's, one MMU model over): everything below
// runs at VMA=LMA in .boot; it programs MAIR/TCR, loads assembly-time
// constant root tables into TTBR0 (identity, to survive the enable)
// and TTBR1 (the HHDM), flips SCTLR_EL1.M, and long-jumps to
// _start_high in the higher-half .text.  The kernel never runs low
// again; paging_a64.c's final tables later drop TTBR0 entirely.
//
// The early tables are DATA, filled at assembly time: 1 GiB block
// leaves are just constants, so no code has to build tables before an
// allocator exists.
//   TTBR0 [1]      VA 0x40000000        -> PA 0x40000000 (identity 1 GiB)
//              -- covers the instructions between the SCTLR write and
//              the jump high; the final tables DROP it (A3 gate).
//   TTBR1 [256-259] VA HHDM+0..4 GiB    -> PA 0..4 GiB
//              -- the D3 direct map: RAM and MMIO for the early
//              kernel, all with AF set (no Access-flag faults), MAIR
//              index 0 (Device-nGnRnE) -- honest for MMIO, merely slow
//              for RAM until the final tables re-attribute it.
//   Level-1 index arithmetic: with T1SZ=25 the TTBR1 window is
//   512 GiB at 0xFFFFFF8000000000; HHDM = that + 256 GiB = index 256
//   -- the SAME index and the SAME constant Sv39 produces.  D3's
//   equal-by-computation, visible right here in the two .fill lines.
//
// Block descriptor bits: [0]=valid, [1]=0 (block), AF=[10],
// SH=[9:8]=11 (inner shareable), AP=[7:6]=00 (EL1 RW), AttrIdx=[4:2].
//   0x40000000 | 0x701 = PA | AF|SH|valid, AttrIdx 0.
// ============================================================================

.section .boot.data
.align 12                         // 2^12: root tables must be page-aligned
early_ttbr0:
    .quad 0                                  // [0] VA 0..1G: unmapped
    .quad 0x40000000 | (1 << 10) | (3 << 8) | 1   // [1] identity 1G @ 0x40000000
    .fill 510, 8, 0

.align 12
early_ttbr1:
    .fill 256, 8, 0
    .quad 0x00000000 | (1 << 10) | (3 << 8) | 1   // [256] HHDM+0G -> PA 0
    .quad 0x40000000 | (1 << 10) | (3 << 8) | 1   // [257] HHDM+1G -> PA 1G
    .quad 0x80000000 | (1 << 10) | (3 << 8) | 1   // [258] HHDM+2G -> PA 2G
    .quad 0xC0000000 | (1 << 10) | (3 << 8) | 1   // [259] HHDM+3G -> PA 3G
    .fill 252, 8, 0

// Literal pool: higher-half addresses cannot be adr'd from a PC of
// 0x4020xxxx; the jump target and stack pointer travel as data (the
// riscv64 boot_jump_pool, aarch64-flavoured).
.align 3
boot_jump_pool:
    .quad _start_high             // VMA in the higher-half .text
    .quad boot_stack_top          // VMA in the higher-half .bss
secondary_jump_pool:              // P6: read by _secondary_start (low)
    .quad _secondary_high         // VMA of the secondary's high half

// The refusal string lives HERE, in the low .boot world, because the
// refusal path runs before (instead of) the jump high -- an adr from
// .text.boot cannot reach the higher-half .rodata (A3 measured the
// relocation overflow the first time this file linked higher-half).
refusal_msg:
    .asciz "AuraLite aarch64: CurrentEL != EL1; refusing to boot (ARM64_PLAN D1).\n"
.align 2

.section .text.boot
.globl _start
_start:
    // ---- A5a: the Linux arm64 Image header, 64 bytes, doubling as
    // the entry point.  BOTH boot paths execute code0 (the branch
    // below): the ELF path enters at _start because ENTRY() says so,
    // the Image path because the protocol defines entry = image
    // start.  text_offset = 0x200000 makes QEMU place the Image at
    // RAM base + 2 MiB = 0x40200000 -- the ELF link address -- so ONE
    // binary layout serves both paths and every symbol keeps its
    // meaning.  llvm-objcopy -O binary of this ELF IS the Image; no
    // second linker script, no relocation games.
    //   flags = bit0:0 LE, bits[2:1]=01 4K granule, bit3:0 base near
    //   DRAM start.  image_size comes from linker arithmetic
    //   (__image_size in kernela64.ld) and includes .bss per the
    //   protocol ("the amount of memory to reserve").
    b     .Lheader_end            // code0: jump the header
    .word 0                       // code1 (pad; code0's b is 4 bytes)
    .quad 0x200000                // text_offset
    .quad __image_size            // image_size (memory footprint)
    .quad 0x2                     // flags: LE, 4K
    .quad 0, 0, 0                 // res2..res4
    .ascii "ARM\x64"              // magic, offset 56
    .word 0                       // res5: header ends at 64
.Lheader_end:
    // ---- 0. Preserve x0: kmain_a64 reports it -- and since A5a the
    // Image path makes it REAL (x0 = DTB phys, the promise A1
    // measured as Image-only).  Still verified, never trusted. ------
    mov   x19, x0

    // ---- 1. D1: this kernel is an EL1 citizen.  CurrentEL != EL1
    // gets an honest refusal banner, not a limp-along -- the I0
    // lesson, fourth edition. -----------------------------------------
    mrs   x1, CurrentEL
    lsr   x1, x1, #2
    cmp   x1, #1
    b.eq  .Lel_ok

    adr   x2, refusal_msg
    bl    boot_puts
    // No power-off here: PSCI over hvc from EL2 would trap into EL2's
    // own vector (us, with no vector table) -- park instead.
.Lpark:
    wfi
    b     .Lpark

.Lel_ok:
    // ---- 2. Mask D,A,I,F; declare SPSel=1.  MEASURED in A2: QEMU
    // enters this ELF with SPSel=0, so the A0/A1 kernel unknowingly
    // ran on SP_EL0 and the first IRQ landed in the IRQ/SP_EL0 vector
    // row (the row whose tag exists to name exactly this bug).  The
    // vector table's SP_EL0 rows stay panic rows BECAUSE of this
    // line -- and A4's EL0 story gets SP_EL0 back as a free register
    // precisely because the kernel never squats on it. ----------------
    msr   daifset, #0xf
    msr   spsel, #1

    // CPACR_EL1.FPEN := 0b11 -- FPU/SIMD open at both ELs.  The
    // kernel itself is -mgeneral-regs-only, but context_a64.S saves
    // q0-q31 EAGERLY on every switch (the M1 lesson, plan Fact 5.6),
    // and with FPEN at its reset value of 0 the very first stp q0,q1
    // would trap EC 0x07.  Opened here, before any switch exists.
    mov   x1, #(3 << 20)
    msr   cpacr_el1, x1

    // ---- 3. MMU on: MAIR, TCR, the two early roots, SCTLR.M. --------
    // MAIR_EL1: index 0 = Device-nGnRnE (0x00), index 1 = Normal WB
    // inner+outer (0xFF).  Two indices, total (plan Fact 5.2); the
    // early tables use only index 0 -- honest for MMIO, slow-but-
    // correct for RAM; paging_a64.c's final tables use index 1 for
    // RAM and keep index 0 for the MMIO windows.
    mov   x1, #0xFF00
    msr   mair_el1, x1

    // TCR_EL1: T0SZ=25 | T1SZ=25<<16 -- 39-bit VA both halves, the
    // Sv39 geometry by choice (D3); TG0=4K (bits[15:14]=00), TG1=4K
    // (bits[31:30]=10); SH/ORGN/IRGN inner-shareable WB WA both
    // halves (0x3<<12|0x1<<10|0x1<<8, and <<28|<<26|<<24 for TTBR1);
    // IPS=40-bit PA (0b010 << 32).
    ldr   x1, =(25 | (0x1 << 8) | (0x1 << 10) | (0x3 << 12) | (25 << 16) | (0x1 << 24) | (0x1 << 26) | (0x3 << 28) | (0x2 << 30) | (0x2 << 32))
    msr   tcr_el1, x1

    adr   x1, early_ttbr0
    msr   ttbr0_el1, x1
    adr   x1, early_ttbr1
    msr   ttbr1_el1, x1

    // The barrier discipline (plan Fact 5.3), in its first outing:
    // dsb to complete the table/system-register writes, isb to
    // resynchronise the pipeline, THEN the M bit.
    dsb   ish
    isb
    mrs   x1, sctlr_el1
    orr   x1, x1, #1              // M: MMU on
    // Deliberately NOT setting C/I yet: cache enable interacts with
    // MAIR attributes and A3's final tables own that decision; the
    // early window stays uncached-correct.
    msr   sctlr_el1, x1
    isb
    // PC is still 0x4020xxxx -- the TTBR0 identity gigapage carries
    // the next two fetches; nothing else low is ever executed again.

    // ---- 4. Long jump to the higher half. ----------------------------
    adr   x1, boot_jump_pool
    ldr   x2, [x1]                // _start_high (VMA)
    ldr   x3, [x1, #8]            // boot stack top (VMA)
    mov   sp, x3
    br    x2

// boot_puts(x2 = NUL-terminated string): PL011 TX with TXFF polling.
// Lives in .text.boot (not the higher-half .text): the refusal path
// runs before (instead of) the MMU flip, and a bl across the HHDM
// gap does not reach.  Clobbers x2-x5 only.
boot_puts:
    ldr   x3, =0x09000000         // PL011 base (virt board, plan Fact 3)
1:  ldrb  w4, [x2], #1
    cbz   w4, 3f
2:  ldr   w5, [x3, #0x18]         // FR
    tbnz  w5, #5, 2b              // TXFF set -> spin
    str   w4, [x3]                // DR
    b     1b
3:  ret

// ============================================================================
// The higher-half world.  From here every address is HHDM-shaped; adr
// works again (PC and symbols are both up high).
// ============================================================================
.section .text
.globl _start_high
_start_high:
    // ---- 5. Zero .bss (the stack lives there; nothing is ON it yet;
    // x19 is live and must survive -- x1..x3 only). ---------------------
    ldr   x1, =__bss_start
    ldr   x2, =__bss_end
.Lbss_clear:
    cmp   x1, x2
    b.hs  .Lbss_done
    str   xzr, [x1], #8
    b     .Lbss_clear
.Lbss_done:
    // The stack was just zeroed along with the rest of .bss; sp still
    // points at its top and nothing had been pushed.  Re-set anyway so
    // the invariant is visible rather than inherited:
    ldr   x1, =boot_stack_top
    mov   sp, x1

    // ---- 6. kmain_a64(x0_at_entry). -----------------------------------
    mov   x0, x19
    bl    kmain_a64

    // ---- 7. Not reached in a healthy boot. ------------------------------
.Lpark_high:
    wfi
    b     .Lpark_high

.section .rodata

// Physical/virtual layout, exported as data: contract 2 of the shared
// DTB walker (kernel/dt/fdt.h) and paging_a64.c's section table.  The
// pool shape (not bare externs) is riscv64's: the higher-half C
// cannot address absolute low symbols, so the CONTRACT is a data pool
// on every arch -- one walker, one access pattern.
.align 3
// ---- PARITY P6: the PSCI CPU_ON secondary entry ---------------------
// CPU_ON lands each started core here at EL1, MMU off, x0 = context
// (this core's stack TOP, high-half VA, chosen by the boot CPU).
// The boot path's moves minus the header dance: DAIF masked, SPSel=1,
// FPEN open, the SAME early roots, SCTLR.M, long jump high.  x19
// carries the stack across the enable (x0 is clobber-prone).
.section .text.boot
.globl _secondary_start
_secondary_start:
    mov   x19, x0
    msr   daifset, #0xf
    msr   spsel, #1
    mov   x1, #(3 << 20)
    msr   cpacr_el1, x1
    mov   x1, #0xFF00
    msr   mair_el1, x1
    ldr   x1, =(25 | (0x1 << 8) | (0x1 << 10) | (0x3 << 12) | (25 << 16) | (0x1 << 24) | (0x1 << 26) | (0x3 << 28) | (0x2 << 30) | (0x2 << 32))
    msr   tcr_el1, x1
    adr   x1, early_ttbr0
    msr   ttbr0_el1, x1
    adr   x1, early_ttbr1
    msr   ttbr1_el1, x1
    dsb   ish
    isb
    mrs   x1, sctlr_el1
    orr   x1, x1, #1
    msr   sctlr_el1, x1
    isb
    adr   x1, secondary_jump_pool // low PC-relative -> identity LMA
    ldr   x2, [x1]
    br    x2

// The secondary's higher half: stack from x19, then C; park after.
.section .text
.globl _secondary_high
_secondary_high:
    mov   sp, x19
    bl    secondary_main_a64
.Lsec_park:
    wfi
    b     .Lsec_park

.section .rodata
.globl kernel_layout
kernel_layout:
    .quad __kernel_start          // [0] image start, PHYS
    .quad __boot_end_phys         // [1] end of .boot, PHYS
    .quad __text_start            // [2] .text start, VMA
    .quad __text_end              // [3]
    .quad __rodata_start          // [4]
    .quad __rodata_end            // [5]
    .quad __data_start            // [6] .data start, VMA
    .quad __kernel_end            // [7] image end, PHYS
    .quad _secondary_start        // [8] P6: PSCI CPU_ON entry, PHYS

.section .bss
.align 16
boot_stack_bottom:
    .space 64 * 1024
.globl boot_stack_top
boot_stack_top:
