; =============================================================================
; fork_return.asm — enter user mode for a fork()/clone() child.
;
; C prototype:
;   void fork_child_sysret(uint64_t rip, uint64_t rflags, uint64_t rsp,
;                          uint64_t rbx, uint64_t rbp, uint64_t r12,
;                          uint64_t r13, uint64_t r14, uint64_t r15);
; SysV argument registers: rdi=rip rsi=rflags rdx=rsp rcx=rbx r8=rbp r9=r12,
;                          [rsp+8]=r13 [rsp+16]=r14 [rsp+24]=r15
;
; The child returns from fork() with RAX=0.  We set up the SYSRET frame:
;   RCX = user RIP, R11 = user RFLAGS, RSP = user RSP, RAX = 0.
; Then execute o64 sysret.
;
; Q12 (POSIX2024_PLAN.md phase Q12): the callee-saved user registers are part
; of the fork() contract.  The SysV syscall ABI promises rbx/rbp/r12-r15
; survive the SYSCALL itself, so compilers keep live values in them across
; fork().  Before Q12 the child resumed with only rip/rflags/rsp/rax restored
; and read kernel leftovers out of rbx..r15 (posix_spawn's attr pointer
; landed in a kernel address and the child faulted before execve).
; The parent returns through the normal sysret path, which republishes the
; same registers from the TCB; here we hand them to the child explicitly.
; Caller-saved GPRs are legitimately dead across the syscall in both parent
; and child, so only RAX (fork return = 0) is set.
; =============================================================================

bits 64
default rel

section .text
global fork_child_sysret

fork_child_sysret:
    mov r10, rsp              ; kernel stack, for the three stacked args
    mov rax, rdx              ; keep user RSP aside (rdx is scratch below)
    mov rbx, rcx              ; user callee-saved registers
    mov rbp, r8
    mov r12, r9
    mov r13, [r10 + 8]
    mov r14, [r10 + 16]
    mov r15, [r10 + 24]
    mov rcx, rdi              ; user RIP (SYSRET loads RIP from RCX)
    mov r11, rsi              ; user RFLAGS (SYSRET loads RFLAGS from R11)
    mov rsp, rax              ; switch to the user stack
    xor rax, rax              ; fork() returns 0 in the child
    o64 sysret                ; 64-bit SYSRET -> Ring 3
