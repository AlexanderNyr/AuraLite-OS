# =============================================================================
# kernel/arch/riscv64/context_rv.S -- kernel context switch (RISCV_PLAN V4).
#
# context32.asm's LP64 sibling: save the callee-saved world on the old
# stack, store the old sp, load the new sp, restore, ret.  The psABI
# callee-saved set on this ISA: ra, s0-s11 -- 13 registers, padded to
# 14 slots (112 bytes) to keep sp 16-byte aligned.
#
# void context_switch_rv(uint64_t *save_sp, uint64_t new_sp);
#                        a0 = &old->sp     a1 = new sp value
#
# A fresh thread's stack is fabricated by thread_rv.c to look exactly
# like this frame with ra = thread_entry_rv, so the first "restore"
# returns into the trampoline.
# =============================================================================

.option norvc

.section .text
.globl context_switch_rv
context_switch_rv:
    addi  sp, sp, -112
    sd    ra,  0(sp)
    sd    s0,  8(sp)
    sd    s1,  16(sp)
    sd    s2,  24(sp)
    sd    s3,  32(sp)
    sd    s4,  40(sp)
    sd    s5,  48(sp)
    sd    s6,  56(sp)
    sd    s7,  64(sp)
    sd    s8,  72(sp)
    sd    s9,  80(sp)
    sd    s10, 88(sp)
    sd    s11, 96(sp)

    sd    sp, 0(a0)               # *save_sp = old sp
    mv    sp, a1                  # sp = new sp

    ld    ra,  0(sp)
    ld    s0,  8(sp)
    ld    s1,  16(sp)
    ld    s2,  24(sp)
    ld    s3,  32(sp)
    ld    s4,  40(sp)
    ld    s5,  48(sp)
    ld    s6,  56(sp)
    ld    s7,  64(sp)
    ld    s8,  72(sp)
    ld    s9,  80(sp)
    ld    s10, 88(sp)
    ld    s11, 96(sp)
    addi  sp, sp, 112
    ret

# =============================================================================
# void user_enter_rv(uint64_t entry, uint64_t user_sp,
#                    uint64_t trap_stack_top)  -- never returns.
#
# The sret into U-mode.  Order is load-bearing:
#   1. SIE off -- between arming sscratch and sret, an S-mode
#      interrupt would take trapentry's swap-and-test down the WRONG
#      branch (sscratch non-zero reads as "arrived from U").  With
#      SIE clear the window contains no trap.
#   2. sscratch := the image's dedicated trap stack (the I7 esp0
#      lesson, this ISA's spelling).
#   3. sstatus.SPP := 0 (sret goes to U), SPIE := 1 (U runs with
#      interrupts on -- the timer keeps preempting).
#   4. sp := user stack, sret.
# =============================================================================
.globl user_enter_rv
user_enter_rv:
    csrci sstatus, 2              # 1. SIE off
    csrw  sscratch, a2            # 2. arm the trap stack
    csrw  sepc, a0
    li    t0, 0x100
    csrc  sstatus, t0             # 3. SPP = 0: sret -> U-mode
    li    t0, 0x20
    csrs  sstatus, t0             #    SPIE = 1: U-mode gets interrupts
    mv    sp, a1                  # 4. the user stack
    sret
