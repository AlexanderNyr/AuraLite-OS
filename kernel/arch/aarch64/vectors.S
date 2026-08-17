// ============================================================================
// kernel/arch/aarch64/vectors.S -- EL1 exception vectors (ARM64_PLAN A2).
//
// VBAR_EL1 points at a TABLE OF CODE, not a table of pointers (plan
// Fact 5.5): 16 slots of 128 bytes each, 2048-byte aligned -- 4
// exception kinds (synchronous / IRQ / FIQ / SError) x 4 origins
// (current EL on SP_EL0, current EL on SP_ELx, lower EL AArch64,
// lower EL AArch32).  128 bytes is not enough for a spill, so every
// slot is a tagged jump to the one shared path -- the trapentry.S
// discipline (one assembly entry, C dispatch), shaped by the table
// instead of by stvec.
//
// A2 runs entirely at EL1 on SP_EL1: the EL0 rows park a kind tag
// too, but the eret side of that story is A4's (SPSel/SP_EL0
// arrangement, the esp0/sscratch lesson's fourth edition -- the plan
// names it there, not here).
//
// Frame (built on the interrupted stack, 288 bytes -- deliberately
// the same size as riscv64's, the registers just fit):
//   x0..x29 at N*8, x30 at 240, ELR_EL1 at 248, SPSR_EL1 at 256,
//   kind tag at 264, [272..288) pad to keep sp 16-aligned (AAPCS64
//   demands it INSIDE the kernel too).
// ============================================================================

// Tags for the 16 slots: origin*4 + kind -- POSITION-major, the same
// arithmetic the hardware's own offset layout uses (origin selects
// the 0x200 block, kind selects the 0x80 slot), so tag == slot index
// and trap_a64.c's kind_names[] reads straight down the table.  (The
// first A2 boot had kind*4+origin here while the dispatcher assumed
// this formula: the self-test UDF landed in SYNC/SP_ELx and was
// dispatched as if it were another row entirely.  One formula, used
// twice, or none.)
#define K_SYNC  0
#define K_IRQ   1
#define K_FIQ   2
#define K_SERR  3
#define O_SP0   0
#define O_SPX   1
#define O_A64   2
#define O_A32   3

.macro VECTOR_SLOT kind, origin
    .balign 128
    stp   x0, x1, [sp, #-288]!      // open the frame; park x0/x1 first
    mov   x0, #(\origin * 4 + \kind)
    b     a64_trap_common
.endm

.section .text
.balign 2048
.globl a64_vectors
a64_vectors:
    // ---- current EL, SP_EL0 (we never run on SP_EL0 in the kernel;
    // arriving here IS the bug the tag names) ----
    VECTOR_SLOT K_SYNC, O_SP0
    VECTOR_SLOT K_IRQ,  O_SP0
    VECTOR_SLOT K_FIQ,  O_SP0
    VECTOR_SLOT K_SERR, O_SP0
    // ---- current EL, SP_ELx: the kernel's own traps ----
    VECTOR_SLOT K_SYNC, O_SPX
    VECTOR_SLOT K_IRQ,  O_SPX
    VECTOR_SLOT K_FIQ,  O_SPX
    VECTOR_SLOT K_SERR, O_SPX
    // ---- lower EL, AArch64: EL0 traps (A4 arms this world) ----
    VECTOR_SLOT K_SYNC, O_A64
    VECTOR_SLOT K_IRQ,  O_A64
    VECTOR_SLOT K_FIQ,  O_A64
    VECTOR_SLOT K_SERR, O_A64
    // ---- lower EL, AArch32: refused by D1's grandchild -- no
    // AArch32 EL0 will ever be entered; the tag makes it a named
    // panic instead of a mystery ----
    VECTOR_SLOT K_SYNC, O_A32
    VECTOR_SLOT K_IRQ,  O_A32
    VECTOR_SLOT K_FIQ,  O_A32
    VECTOR_SLOT K_SERR, O_A32

// ---- the one shared path ---------------------------------------------------
// On entry: frame open (sp dropped 288), x0/x1 parked at [sp], x0 =
// kind tag.  Spill the rest, hand the frame to C.
a64_trap_common:
    stp   x2,  x3,  [sp, #16]
    stp   x4,  x5,  [sp, #32]
    stp   x6,  x7,  [sp, #48]
    stp   x8,  x9,  [sp, #64]
    stp   x10, x11, [sp, #80]
    stp   x12, x13, [sp, #96]
    stp   x14, x15, [sp, #112]
    stp   x16, x17, [sp, #128]
    stp   x18, x19, [sp, #144]
    stp   x20, x21, [sp, #160]
    stp   x22, x23, [sp, #176]
    stp   x24, x25, [sp, #192]
    stp   x26, x27, [sp, #208]
    stp   x28, x29, [sp, #224]
    str   x30,      [sp, #240]

    mrs   x1, elr_el1
    str   x1, [sp, #248]
    mrs   x1, spsr_el1
    str   x1, [sp, #256]
    str   x0, [sp, #264]            // the kind tag

    // a64_trap(frame) -- may rewrite ELR (resume past a fault) or,
    // later, x0 (A4's syscall return lives in the frame's [0]).
    mov   x0, sp
    bl    a64_trap

    // ---- exit: restore in reverse; ELR/SPSR first (they need a
    // scratch register that is restored after them) ----
    ldr   x1, [sp, #248]
    msr   elr_el1, x1
    ldr   x1, [sp, #256]
    msr   spsr_el1, x1

    ldp   x2,  x3,  [sp, #16]
    ldp   x4,  x5,  [sp, #32]
    ldp   x6,  x7,  [sp, #48]
    ldp   x8,  x9,  [sp, #64]
    ldp   x10, x11, [sp, #80]
    ldp   x12, x13, [sp, #96]
    ldp   x14, x15, [sp, #112]
    ldp   x16, x17, [sp, #128]
    ldp   x18, x19, [sp, #144]
    ldp   x20, x21, [sp, #160]
    ldp   x22, x23, [sp, #176]
    ldp   x24, x25, [sp, #192]
    ldp   x26, x27, [sp, #208]
    ldp   x28, x29, [sp, #224]
    ldr   x30,      [sp, #240]
    ldp   x0,  x1,  [sp], #288      // close the frame last
    eret

// ============================================================================
// a64_setjmp / a64_longjmp_entry -- the A3 fault-probe mechanism (the
// rv_setjmp pair, AAPCS64-flavoured; A4 will reuse it as the EL0
// image's exit trampoline exactly as V4 reused rv_setjmp).
//
// A deliberate fault probe (store to .text, execute from data) cannot
// be resumed by elr += 4: for execute-from-data, elr points INTO the
// data buffer and the next word is as unexecutable as the first.  The
// handler instead teleports back: a64_setjmp captures the callee-saved
// world before the probe, and on an EXPECTED fault trap_a64.c rewrites
// the frame so eret lands in a64_longjmp_entry with x0 = the buf --
// which restores that world and "returns" 1 from a64_setjmp.
//
// buf layout: [0]=x30/lr [1]=sp [2..13]=x19..x29 pairs (14 quads, 112
// bytes -- the same size as the rv buf; the callee-saved sets happen
// to match 1:1).
// ============================================================================
.globl a64_setjmp
a64_setjmp:
    mov   x1, sp
    stp   x30, x1,  [x0]
    stp   x19, x20, [x0, #16]
    stp   x21, x22, [x0, #32]
    stp   x23, x24, [x0, #48]
    stp   x25, x26, [x0, #64]
    stp   x27, x28, [x0, #80]
    str   x29,      [x0, #96]
    mov   x0, #0
    ret

.globl a64_longjmp_entry
a64_longjmp_entry:
    ldp   x30, x1,  [x0]
    mov   sp, x1
    ldp   x19, x20, [x0, #16]
    ldp   x21, x22, [x0, #32]
    ldp   x23, x24, [x0, #48]
    ldp   x25, x26, [x0, #64]
    ldp   x27, x28, [x0, #80]
    ldr   x29,      [x0, #96]
    mov   x0, #1
    ret
