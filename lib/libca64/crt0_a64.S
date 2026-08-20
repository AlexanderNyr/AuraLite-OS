// ============================================================================
// lib/libca64/crt0_a64.S -- aarch64 user program entry (ARM64_PLAN A5b).
// crt0_rv.S's sibling at the fourth trap mechanism: call main, feed the
// return value to SYS_EXIT, never fall through.  The exit trap is
// inlined rather than calling libc's exit() -- libca64's functions are
// static inline (no archive to link), and the one thing a crt0 must
// never depend on is the layout of a library that might not be there
// (the I5 rule, fourth verse).
//
// The D4 accident holds a fourth time: main's return value arrives in
// x0 and SYS_EXIT's argument leaves in x0 -- the register is already
// right, only x8 needs loading.  main() is main(void) at this phase;
// argv/envp arrive with the full process model.
// ============================================================================

.section .text
.globl _start
_start:
    mov   x29, xzr               // mark the outermost frame
    mov   x30, xzr
    bl    main
    mov   x8, #60                // SYS_EXIT (AuraLite numbers, plan D4)
    svc   #0                     // x0 = main's return, already in place
1:                               // unreachable: SYS_EXIT does not return
    wfi
    b     1b
