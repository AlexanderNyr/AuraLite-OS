; =============================================================================
; lib/libc32/crt0_32.asm -- i386 user program entry (I386_PLAN I5).
; Sibling of lib/libc/crt/crt0.asm one width down: call main, feed the
; return value to SYS_EXIT, never fall through.  The exit trap is
; inlined here rather than calling the libc's exit() -- libc32's
; functions are static inline (there is no archive to link yet), and
; the one thing a crt0 must never depend on is the layout of a library
; that might not be there.  argv/envp arrive with the full process
; model (I6+); main() at this phase is main(void).
; =============================================================================
bits 32

SYS_EXIT equ 60                 ; AuraLite numbers (plan D4)

section .text
global _start
extern main

_start:
    xor  ebp, ebp               ; mark the outermost frame
    call main
    mov  ebx, eax               ; exit code = main's return
    mov  eax, SYS_EXIT
    int  0x80
.hang:                          ; unreachable: SYS_EXIT does not return
    hlt
    jmp  .hang
