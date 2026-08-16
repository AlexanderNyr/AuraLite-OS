; =============================================================================
; lib/libc32/syscall32.asm -- the int 0x80 wrapper (I386_PLAN I5).
; Sibling of lib/libc/src/syscall.asm one width down.
;
; long __syscall32(long n, long a1, long a2, long a3, long a4, long a5);
;
; cdecl in, D4 register convention out: EAX = number, then
; EBX, ECX, EDX, ESI, EDI.  EBX/ESI/EDI are callee-saved under cdecl,
; so they are preserved around the trap.  The 6th argument register
; (EBP) is deliberately not marshalled until something needs it --
; every I5 syscall takes <= 3 arguments.
; =============================================================================
bits 32

section .text
global __syscall32
__syscall32:
    push ebx
    push esi
    push edi

    mov  eax, [esp + 16]        ; n    (3 saves + ret = 16)
    mov  ebx, [esp + 20]        ; a1
    mov  ecx, [esp + 24]        ; a2
    mov  edx, [esp + 28]        ; a3
    mov  esi, [esp + 32]        ; a4
    mov  edi, [esp + 36]        ; a5

    int  0x80

    pop  edi
    pop  esi
    pop  ebx
    ret
