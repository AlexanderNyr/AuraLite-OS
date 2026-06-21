; =============================================================================
; crt0.asm — C runtime entry point for AuraLite OS user programs.
;
; The kernel loads the ELF and jumps to _start with RSP pointing to the top of
; a mapped user stack (16-byte aligned). We clear the frame pointer, call main,
; and then exit with its return value via SYS_EXIT.
; =============================================================================

bits 64
default rel

section .text
global _start
extern main

_start:
    xor rbp, rbp              ; clear frame pointer (ABI requirement)
    call main                 ; RSP is 16-aligned; call pushes ret addr -> RSP≡8
    mov edi, eax              ; exit code from main's return value
    mov eax, 60               ; SYS_EXIT
    syscall
    ; Should never reach here — SYS_EXIT does not return.
    ud2
