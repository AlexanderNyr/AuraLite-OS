; =============================================================================
; fork_return.asm — return to user mode for a fork() child.
;
; C prototype: void fork_child_sysret(uint64_t rip, uint64_t rflags, uint64_t rsp);
;   rdi = user RIP (where to resume — the instruction after the syscall)
;   rsi = user RFLAGS
;   rdx = user RSP
;
; The child returns from fork() with RAX=0. We set up the SYSRET frame:
;   RCX = user RIP, R11 = user RFLAGS, RSP = user RSP, RAX = 0.
; Then execute o64 sysret.
; =============================================================================

bits 64
default rel

section .text
global fork_child_sysret

fork_child_sysret:
    mov rsp, rdx              ; switch to the user stack
    mov rcx, rdi              ; user RIP (SYSRET loads RIP from RCX)
    mov r11, rsi              ; user RFLAGS (SYSRET loads RFLAGS from R11)
    xor rax, rax              ; fork() returns 0 in the child
    o64 sysret                ; 64-bit SYSRET -> Ring 3
