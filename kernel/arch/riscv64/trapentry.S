# =============================================================================
# kernel/arch/riscv64/trapentry.S -- S-mode trap entry/exit (RISCV_PLAN V2).
#
# stvec points here (direct mode: all causes, one vector -- the low two
# bits of stvec select the mode, so this symbol must be 4-byte aligned).
#
# Frame: x1..x31 stored at (N-1)*8, sepc at 248 -- 288 bytes total to
# keep sp 16-byte aligned (the psABI stack rule holds INSIDE the
# kernel too; LLVM emits 16-byte-aligned spills against it).
#
# V2 scope: traps arrive from S-mode only (no U-mode until V4), so the
# interrupted sp IS a kernel sp and pushing the frame right onto it is
# correct.  V4 adds the sscratch swap for U-mode entry -- the plan
# names that stack-switch as I7's trap-stack lesson, third verse.
#
# x2 (sp) is not stored through the loop: its "saved" value is just
# sp+288, recomputed at exit by the addi.  The slot at offset 8 is
# still written (debuggers walking the frame want to see it) but never
# reloaded.
# =============================================================================

.option norvc

.section .text
.align 4
.globl rv_trap_vector
rv_trap_vector:
    addi  sp, sp, -288

    sd    x1,  0(sp)
    # x2's slot: the pre-trap sp, for the frame dump's benefit.
    addi  x1, sp, 288
    sd    x1,  8(sp)
    sd    x3,  16(sp)
    sd    x4,  24(sp)
    sd    x5,  32(sp)
    sd    x6,  40(sp)
    sd    x7,  48(sp)
    sd    x8,  56(sp)
    sd    x9,  64(sp)
    sd    x10, 72(sp)
    sd    x11, 80(sp)
    sd    x12, 88(sp)
    sd    x13, 96(sp)
    sd    x14, 104(sp)
    sd    x15, 112(sp)
    sd    x16, 120(sp)
    sd    x17, 128(sp)
    sd    x18, 136(sp)
    sd    x19, 144(sp)
    sd    x20, 152(sp)
    sd    x21, 160(sp)
    sd    x22, 168(sp)
    sd    x23, 176(sp)
    sd    x24, 184(sp)
    sd    x25, 192(sp)
    sd    x26, 200(sp)
    sd    x27, 208(sp)
    sd    x28, 216(sp)
    sd    x29, 224(sp)
    sd    x30, 232(sp)
    sd    x31, 240(sp)

    csrr  t0, sepc
    sd    t0, 248(sp)

    # rv_trap(frame) -- may rewrite frame->sepc to resume PAST a
    # deliberate fault (the [isr] self-test's whole mechanism).
    mv    a0, sp
    call  rv_trap

    ld    t0, 248(sp)
    csrw  sepc, t0

    ld    x1,  0(sp)
    ld    x3,  16(sp)
    ld    x4,  24(sp)
    ld    x5,  32(sp)
    ld    x6,  40(sp)
    ld    x7,  48(sp)
    ld    x8,  56(sp)
    ld    x9,  64(sp)
    ld    x10, 72(sp)
    ld    x11, 80(sp)
    ld    x12, 88(sp)
    ld    x13, 96(sp)
    ld    x14, 104(sp)
    ld    x15, 112(sp)
    ld    x16, 120(sp)
    ld    x17, 128(sp)
    ld    x18, 136(sp)
    ld    x19, 144(sp)
    ld    x20, 152(sp)
    ld    x21, 160(sp)
    ld    x22, 168(sp)
    ld    x23, 176(sp)
    ld    x24, 184(sp)
    ld    x25, 192(sp)
    ld    x26, 200(sp)
    ld    x27, 208(sp)
    ld    x28, 216(sp)
    ld    x29, 224(sp)
    ld    x30, 232(sp)
    ld    x31, 240(sp)

    addi  sp, sp, 288
    sret
