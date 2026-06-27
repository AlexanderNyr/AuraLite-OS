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
extern exit
extern __stack_chk_guard

_start:
    ; Per-process stack protector seed. Uses the initial stack pointer and the
    ; guard symbol address so every process instance gets a different cookie.
    lea rax, [rel __stack_chk_guard]
    xor rax, rsp
    mov rdx, 0x6B1F2D4CA53E9071
    xor rax, rdx
    mov [rel __stack_chk_guard], rax

    xor rbp, rbp              ; clear frame pointer (ABI requirement)
    call main                 ; RSP is 16-aligned; call pushes ret addr -> RSP≡8
    mov edi, eax              ; exit code from main's return value
    call exit                 ; libc exit(): flush stdio buffers, then SYS_EXIT
    ; Should never reach here — exit() does not return.
    ud2
