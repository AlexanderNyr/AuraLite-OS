// ============================================================================
// kernel/arch/aarch64/context_a64.S -- kernel context switch (ARM64_PLAN A4).
//
// context_rv.S's AAPCS64 sibling: save the callee-saved world on the
// old stack, store the old sp, load the new sp, restore, ret.  The
// callee-saved set here: x19-x28, x29 (fp), x30 (lr) -- 12 registers,
// even, so the integer frame is 96 bytes with no pad.
//
// THE FPU FRAME (plan Fact 5.6, the M1 lesson pre-paid): 32 x 128-bit
// q registers + fpcr + fpsr = 528 bytes, saved EAGERLY on every
// switch.  The kernel itself is -mgeneral-regs-only, so why save at
// all?  Because EL0 images use the FPU freely (A4 opens
// CPACR_EL1.FPEN), an EL0 image's FPU state lives across ITS kernel
// thread's suspensions, and the trap-and-lazy-save tradition is
// refused HERE for the same measured reason the rv64 and x86_64 ports
// refused it: the M1 corruption was a lazy-save bug.  Total frame:
// 96 + 528 = 624 bytes, 16-aligned.
//
// void context_switch_a64(uint64_t *save_sp, uint64_t new_sp);
//                         x0 = &old->sp     x1 = new sp value
//
// A fresh thread's stack is fabricated by thread_a64.c to look
// exactly like this frame with lr = thread_entry_a64, q-block zero,
// so the first "restore" returns into the trampoline (and restoring
// 528 zero bytes of FPU state is the correct initial state).
// ============================================================================

// The kernel's CFLAGS say -mgeneral-regs-only: the COMPILER must not
// touch q-registers (no context saves them behind our back).  This
// file is the one place that touches them ON PURPOSE -- the
// directives open the assembler's gate without loosening the
// compiler's flag.
.arch_extension fp
.arch_extension simd

.section .text
.globl context_switch_a64
context_switch_a64:
    sub   sp, sp, #624

    // ---- integer callee-saved: [0..96) ----
    stp   x19, x20, [sp, #0]
    stp   x21, x22, [sp, #16]
    stp   x23, x24, [sp, #32]
    stp   x25, x26, [sp, #48]
    stp   x27, x28, [sp, #64]
    stp   x29, x30, [sp, #80]

    // ---- FPU block: fpcr/fpsr at [96..112) (stp-x reach ends at
    // #504, so the control pair sits LOW), q0-q31 at [112..624) ----
    mrs   x2, fpcr
    mrs   x3, fpsr
    stp   x2, x3, [sp, #96]
    stp   q0,  q1,  [sp, #112]
    stp   q2,  q3,  [sp, #144]
    stp   q4,  q5,  [sp, #176]
    stp   q6,  q7,  [sp, #208]
    stp   q8,  q9,  [sp, #240]
    stp   q10, q11, [sp, #272]
    stp   q12, q13, [sp, #304]
    stp   q14, q15, [sp, #336]
    stp   q16, q17, [sp, #368]
    stp   q18, q19, [sp, #400]
    stp   q20, q21, [sp, #432]
    stp   q22, q23, [sp, #464]
    stp   q24, q25, [sp, #496]
    stp   q26, q27, [sp, #528]
    stp   q28, q29, [sp, #560]
    stp   q30, q31, [sp, #592]

    mov   x2, sp
    str   x2, [x0]                // *save_sp = old sp
    mov   sp, x1                  // sp = new sp

    ldp   x2, x3, [sp, #96]
    msr   fpcr, x2
    msr   fpsr, x3
    ldp   q0,  q1,  [sp, #112]
    ldp   q2,  q3,  [sp, #144]
    ldp   q4,  q5,  [sp, #176]
    ldp   q6,  q7,  [sp, #208]
    ldp   q8,  q9,  [sp, #240]
    ldp   q10, q11, [sp, #272]
    ldp   q12, q13, [sp, #304]
    ldp   q14, q15, [sp, #336]
    ldp   q16, q17, [sp, #368]
    ldp   q18, q19, [sp, #400]
    ldp   q20, q21, [sp, #432]
    ldp   q22, q23, [sp, #464]
    ldp   q24, q25, [sp, #496]
    ldp   q26, q27, [sp, #528]
    ldp   q28, q29, [sp, #560]
    ldp   q30, q31, [sp, #592]

    ldp   x19, x20, [sp, #0]
    ldp   x21, x22, [sp, #16]
    ldp   x23, x24, [sp, #32]
    ldp   x25, x26, [sp, #48]
    ldp   x27, x28, [sp, #64]
    ldp   x29, x30, [sp, #80]
    add   sp, sp, #624
    ret

// ============================================================================
// void user_enter_a64(uint64_t entry, uint64_t user_sp,
//                     uint64_t trap_stack_top)  -- never returns.
//
// The eret into EL0.  Order is load-bearing (user_enter_rv's, DAIF-
// flavoured):
//   1. IRQ masked -- between arming SP_EL1-for-EL0 and eret, an EL1
//      interrupt would build its frame on the WRONG stack notion.
//      (QEMU delivered one on the first unmasked cut of the rv
//      sibling; the lesson transfers by lineage, not re-measurement.)
//   2. sp := the image's dedicated trap stack.  THE SPSel TWIST: on
//      riscv the trap stack travels in a scratch CSR; here the
//      hardware switches to SP_EL1 on any EL0 trap, so "arm the trap
//      stack" simply means "leave it IN sp before dropping down" --
//      the A2 SPSel=1 discipline is what makes sp and SP_EL1 the
//      same register right now.  No CSR, no swap-and-test: the I7
//      esp0 contract is a hardware feature on this ISA.
//   3. SPSR_EL1 := EL0t with IRQ unmasked (the image runs with the
//      timer preempting it), ELR := entry.
//   4. SP_EL0 := user stack (free real estate -- the kernel never
//      squats on it, the A2 measured fact), eret.
// ============================================================================
.globl user_enter_a64
user_enter_a64:
    msr   daifset, #2             // 1. IRQ off across the arming
    mov   sp, x2                  // 2. the dedicated trap stack IS sp
    msr   elr_el1, x0             // 3. where EL0 starts
    mov   x3, #0                  //    SPSR: EL0t, DAIF all clear
    msr   spsr_el1, x3
    msr   sp_el0, x1              // 4. the user stack
    eret
