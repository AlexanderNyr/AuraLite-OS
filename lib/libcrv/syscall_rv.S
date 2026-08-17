# =============================================================================
# lib/libcrv/syscall_rv.S -- the ecall wrapper (RISCV_PLAN V5).
# Sibling of syscall32.asm at the third width.
#
# long __syscall_rv(long n, long a1, long a2, long a3, long a4, long a5);
#
# psABI in, D4 register convention out: the number moves from a0 to
# a7, the five arguments shuffle down one register each.  No stack, no
# callee-saved traffic -- the whole marshalling is six mv instructions
# (the pleasure of an ISA whose call and trap conventions were designed
# by people who had met each other).
# =============================================================================

.option norvc

.section .text
.globl __syscall_rv
__syscall_rv:
    mv    a7, a0
    mv    a0, a1
    mv    a1, a2
    mv    a2, a3
    mv    a3, a4
    mv    a4, a5
    ecall
    ret
