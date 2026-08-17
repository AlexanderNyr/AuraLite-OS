# =============================================================================
# lib/libcrv/crt0_rv.S -- rv64 user program entry (RISCV_PLAN V5).
# Sibling of crt0_32.asm at the third width: call main, feed the return
# value to SYS_EXIT, never fall through.  The exit trap is inlined here
# rather than calling the libc's exit() -- libcrv's functions are
# static inline (there is no archive to link), and the one thing a
# crt0 must never depend on is the layout of a library that might not
# be there (the I5 rule, third verse).
#
# The convenient accident of the D4 convention: main's return value
# arrives in a0, and SYS_EXIT's argument leaves in a0 -- the register
# is already right, only a7 needs loading.  argv/envp arrive with the
# full process model; main() at this phase is main(void).
# =============================================================================

.option norvc

.section .text
.globl _start
_start:
    li    s0, 0                  # mark the outermost frame
    li    ra, 0
    call  main
    li    a7, 60                 # SYS_EXIT (AuraLite numbers, plan D4)
    ecall                        # a0 = main's return, already in place
1:                               # unreachable: SYS_EXIT does not return
    wfi
    j     1b
