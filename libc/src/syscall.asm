; =============================================================================
; syscall.asm — generic syscall wrapper for AuraLite OS user programs.
;
; C prototype:
;   long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6);
;
; C ABI (System V AMD64):  rdi=num rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5 [stack]=a6
; SYSCALL ABI:             rax=num rdi=a1 rsi=a2 rdx=a3 r10=a4 r8=a5 r9=a6
;
; We remap registers before the `syscall` instruction. The SYSCALL instruction
; clobbers RCX (saves RIP) and R11 (saves RFLAGS), so we move rcx to r10 first.
; =============================================================================

bits 64
default rel

section .text
global syscall

syscall:
    mov rax, rdi              ; num
    mov rdi, rsi              ; a1
    mov rsi, rdx              ; a2
    mov rdx, rcx              ; a3 (safe: syscall clobbers rcx AFTER we read it)
    mov r10, r8               ; a4
    mov r8,  r9               ; a5
    mov r9,  [rsp + 8]        ; a6 (first stack arg past the return address)
    syscall
    ret
