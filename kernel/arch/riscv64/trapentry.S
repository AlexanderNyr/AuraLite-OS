# =============================================================================
# kernel/arch/riscv64/trapentry.S -- S-mode trap entry/exit (RISCV_PLAN V2;
# U-mode aware since V4).
#
# stvec points here (direct mode: all causes, one vector -- the low two
# bits of stvec select the mode, so this symbol must be 4-byte aligned).
#
# Frame: x1..x31 stored at (N-1)*8, sepc at 248, sstatus at 256 --
# 288 bytes total to keep sp 16-byte aligned (the psABI stack rule
# holds INSIDE the kernel too).
#
# The V4 stack switch -- the I7 esp0 lesson, solved the way this ISA
# solves it: sscratch is 0 while the hart runs in S-mode, and holds
# the dedicated trap-stack top while it runs in U-mode (armed by
# user_enter_rv).  Entry does csrrw-swap-and-test:
#
#   from S: sscratch was 0 -> swap back, build the frame on the
#           interrupted kernel sp (correct: it IS a kernel stack);
#   from U: sscratch held the trap stack -> sp lands on it, the USER
#           sp is parked in sscratch for the x2 slot, then sscratch
#           is zeroed ("we are in the kernel now").
#
# Exit re-arms: if the sstatus image being restored has SPP=0 (going
# back to U-mode), sscratch := this frame's base + 288 == the trap
# stack top it came in on.  The x2 slot is restored LAST via
# `ld sp, 8(sp)` -- one sequence serves both worlds because the slot
# holds "sp+288" for S-traps and the user sp for U-traps.
# =============================================================================

.option norvc

.section .text
.align 4
.globl rv_trap_vector
rv_trap_vector:
    csrrw sp, sscratch, sp        # swap: sp <- (0 | trap stack)
    bnez  sp, .Lstack_switched    # non-zero: arrived from U-mode
    csrrw sp, sscratch, sp        # S-mode trap: undo, stay on kernel sp
.Lstack_switched:
    addi  sp, sp, -288

    sd    x1,  0(sp)
    # x2's slot: the interrupted sp.  From U it is parked in sscratch
    # by the swap above; from S sscratch reads 0 and it is sp+288.
    csrr  x1, sscratch
    bnez  x1, 1f
    addi  x1, sp, 288
1:
    sd    x1,  8(sp)
    csrw  sscratch, x0            # in the kernel now: S-trap semantics
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
    csrr  t0, sstatus
    sd    t0, 256(sp)

    # rv_trap(frame) -- may rewrite sepc (resume past a fault), a0
    # (syscall return), or sepc+sstatus together (longjmp unwind out
    # of a finished U-mode image).
    mv    a0, sp
    call  rv_trap

    ld    t0, 256(sp)
    csrw  sstatus, t0
    andi  t1, t0, 0x100           # SPP bit
    bnez  t1, 2f                  # returning to S: sscratch stays 0
    addi  t1, sp, 288             # returning to U: re-arm the trap stack
    csrw  sscratch, t1
2:
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

    ld    sp, 8(sp)               # x2 last: kernel sp+288 OR the user sp
    sret

# =============================================================================
# rv_setjmp / rv_longjmp_entry -- the V3 fault-probe mechanism, reused
# by V4 as the U-mode image's exit trampoline (user32.c's saved_esp/
# saved_eip pair, RISC-V-flavoured).
#
# A deliberate fault probe (store to .text, execute from data) cannot
# be resumed by sepc += 4: for execute-from-data, sepc points INTO the
# data buffer and the next word is as unexecutable as the first.  The
# handler instead teleports back: rv_setjmp captures the callee-saved
# world before the probe, and on an EXPECTED fault trap.c rewrites the
# frame so sret lands in rv_longjmp_entry with a0 = the buf -- which
# restores that world and "returns" 1 from rv_setjmp.  The setjmp
# contract, trap-flavoured.  (For a U-mode exit the rewrite also sets
# the frame's SPP so sret comes back to S-mode; a1 carries the exit
# code and rv_setjmp's second return value is 1 + code.)
#
# buf layout: [0]=ra [1]=sp [2..13]=s0..s11  (112 bytes)
# =============================================================================

.globl rv_setjmp
rv_setjmp:
    sd    ra,  0(a0)
    sd    sp,  8(a0)
    sd    s0,  16(a0)
    sd    s1,  24(a0)
    sd    s2,  32(a0)
    sd    s3,  40(a0)
    sd    s4,  48(a0)
    sd    s5,  56(a0)
    sd    s6,  64(a0)
    sd    s7,  72(a0)
    sd    s8,  80(a0)
    sd    s9,  88(a0)
    sd    s10, 96(a0)
    sd    s11, 104(a0)
    li    a0, 0
    ret

.globl rv_longjmp_entry
rv_longjmp_entry:
    # a0 = buf, a1 = value-1 (planted in the trap frame by trap.c;
    # fault probes pass a1=0 so setjmp returns 1, exactly as before)
    ld    ra,  0(a0)
    ld    sp,  8(a0)
    ld    s0,  16(a0)
    ld    s1,  24(a0)
    ld    s2,  32(a0)
    ld    s3,  40(a0)
    ld    s4,  48(a0)
    ld    s5,  56(a0)
    ld    s6,  64(a0)
    ld    s7,  72(a0)
    ld    s8,  80(a0)
    ld    s9,  88(a0)
    ld    s10, 96(a0)
    ld    s11, 104(a0)
    addi  a0, a1, 1               # setjmp's second return: 1 + value
    ret
