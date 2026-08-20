// ============================================================================
// lib/libca64/syscall_a64.S -- the svc wrapper (ARM64_PLAN A5b).
// syscall_rv.S's sibling at the fourth trap mechanism.
//
// long __syscall_a64(long n, long a1, long a2, long a3, long a4, long a5);
//
// AAPCS64 in, D4 register convention out: the number moves from x0 to
// x8, the five arguments shuffle down one register each.  No stack, no
// callee-saved traffic -- like the rv64 wrapper, the whole marshalling
// is six moves (AAPCS64's argument registers and the kernel's syscall
// registers overlap by design of the convention we chose, not by
// accident of this ISA).
// ============================================================================

.section .text
.globl __syscall_a64
__syscall_a64:
    mov   x8, x0
    mov   x0, x1
    mov   x1, x2
    mov   x2, x3
    mov   x3, x4
    mov   x4, x5
    svc   #0
    ret
